// SPDX-License-Identifier: AGPL-3.0-only
#include "FontLocator.h"
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#ifdef Q_OS_WIN
#include <dwrite.h>
#include <wrl/client.h>
#endif

namespace yolo::platform {
namespace {
QString clean(QString name) {
    if (name.size() > 7 && name[6] == '+') {
        bool prefix = true;
        for (int i = 0; i < 6; ++i)
            prefix = prefix && name[i] >= 'A' && name[i] <= 'Z';
        if (prefix)
            name.remove(0, 7);
    }
    return name;
}
#ifdef Q_OS_ANDROID
quint16 u16(const QByteArray &data, int at) {
    if (at < 0 || at + 2 > data.size())
        return 0;
    return (quint8(data[at]) << 8) | quint8(data[at + 1]);
}
quint32 u32(const QByteArray &data, int at) {
    if (at < 0 || at + 4 > data.size())
        return 0;
    return (quint32(u16(data, at)) << 16) | u16(data, at + 2);
}
QString postscript(const QByteArray &data, quint32 face) {
    if (face > quint32(data.size()) || quint64(face) + 12 > quint64(data.size()))
        return {};
    const int count = u16(data, face + 4);
    for (int i = 0; i < count; ++i) {
        const quint64 entry = quint64(face) + 12 + quint64(i) * 16;
        if (entry + 16 > quint64(data.size()))
            return {};
        if (data.mid(entry, 4) != "name")
            continue;
        const quint32 offset = u32(data, entry + 8), length = u32(data, entry + 12);
        if (quint64(offset) + length > quint64(data.size()) || length < 6)
            return {};
        const int names = u16(data, offset + 2), strings = u16(data, offset + 4);
        for (int j = 0; j < names; ++j) {
            const quint64 record = quint64(offset) + 6 + quint64(j) * 12;
            if (record + 12 > quint64(offset) + length)
                return {};
            if (u16(data, record + 6) != 6)
                continue;
            const int platform = u16(data, record), size = u16(data, record + 8);
            const quint64 start = quint64(offset) + strings + u16(data, record + 10);
            if (start + size > quint64(offset) + length)
                return {};
            if (platform == 0 || platform == 3) {
                QString result;
                for (int k = 0; k + 1 < size; k += 2)
                    result.append(QChar(u16(data, start + k)));
                return result;
            }
            if (platform == 1)
                return QString::fromLatin1(data.constData() + start, size);
        }
    }
    return {};
}
#endif
#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;
QString stringValue(IDWriteLocalizedStrings *strings) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    strings->FindLocaleName(L"en-us", &index, &exists);
    if (!exists)
        index = 0;
    UINT32 length = 0;
    if (FAILED(strings->GetStringLength(index, &length)))
        return {};
    std::wstring value(length + 1, L'\0');
    if (FAILED(strings->GetString(index, value.data(), length + 1)))
        return {};
    return QString::fromWCharArray(value.data(), length);
}
#endif
} // namespace
FontMatch findExactFont(QString requested) {
    requested = clean(requested);
    if (requested.isEmpty())
        return {};
#ifdef Q_OS_WIN
    ComPtr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(factory.GetAddressOf()))))
        return {};
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(collection.GetAddressOf())))
        return {};
    for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i) {
        ComPtr<IDWriteFontFamily> family;
        if (FAILED(collection->GetFontFamily(i, family.GetAddressOf())))
            continue;
        for (UINT32 j = 0; j < family->GetFontCount(); ++j) {
            ComPtr<IDWriteFont> font;
            if (FAILED(family->GetFont(j, font.GetAddressOf())))
                continue;
            ComPtr<IDWriteLocalizedStrings> names;
            BOOL exists = FALSE;
            if (FAILED(font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME,
                                                     names.GetAddressOf(), &exists)) ||
                !exists || stringValue(names.Get()).compare(requested, Qt::CaseInsensitive) != 0)
                continue;
            ComPtr<IDWriteFontFace> face;
            if (FAILED(font->CreateFontFace(face.GetAddressOf())))
                continue;
            UINT32 count = 1;
            ComPtr<IDWriteFontFile> file;
            if (FAILED(face->GetFiles(&count, nullptr)) || count != 1 ||
                FAILED(face->GetFiles(&count, file.GetAddressOf())))
                continue;
            const void *key = nullptr;
            UINT32 size = 0;
            if (FAILED(file->GetReferenceKey(&key, &size)))
                continue;
            ComPtr<IDWriteFontFileLoader> loader;
            if (FAILED(file->GetLoader(loader.GetAddressOf())))
                continue;
            ComPtr<IDWriteLocalFontFileLoader> local;
            if (FAILED(loader.As(&local)))
                continue;
            UINT32 length = 0;
            if (FAILED(local->GetFilePathLengthFromKey(key, size, &length)))
                continue;
            std::wstring path(length + 1, L'\0');
            if (FAILED(local->GetFilePathFromKey(key, size, path.data(), length + 1)))
                continue;
            return {QString::fromWCharArray(path.data(), length), int(face->GetIndex())};
        }
    }
    return {};
#elif defined(Q_OS_ANDROID)
    QDirIterator files("/system/fonts", {"*.ttf", "*.otf", "*.ttc"}, QDir::Files);
    while (files.hasNext()) {
        const auto path = files.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
            continue;
        const auto data = file.readAll();
        const bool collection = data.startsWith("ttcf");
        const quint32 count = collection ? u32(data, 8) : 1;
        if (count > 256)
            continue;
        for (quint32 face = 0; face < count; ++face) {
            const quint32 offset = collection ? u32(data, 12 + face * 4) : 0;
            if (postscript(data, offset).compare(requested, Qt::CaseInsensitive) == 0)
                return {path, int(face)};
        }
    }
    return {};
#else
    QProcess process;
    process.start("fc-match",
                  {"-f", "%{file}\n%{postscriptname}\n%{index}\n", ":postscriptname=" + requested});
    if (!process.waitForFinished(5000) || process.exitCode() != 0)
        return {};
    const auto lines = QString::fromUtf8(process.readAllStandardOutput()).split('\n');
    if (lines.size() < 3 || !lines[1].split(',').contains(requested, Qt::CaseInsensitive) ||
        !QFileInfo(lines[0]).isFile())
        return {};
    bool ok = false;
    const int index = lines[2].toInt(&ok);
    return ok && index >= 0 ? FontMatch{lines[0], index} : FontMatch{};
#endif
}
} // namespace yolo::platform
