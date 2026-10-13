// SPDX-License-Identifier: AGPL-3.0-only
#include "LibraryArchive.h"
#include <QSet>
#include <QtEndian>
#include <stdexcept>
namespace yolo::core {
namespace {
quint32 crc(const QByteArray &data) {
    quint32 value = ~0u;
    for (unsigned char c : data) {
        value ^= c;
        for (int i = 0; i < 8; ++i)
            value = (value >> 1) ^ (0xedb88320u & quint32(-int(value & 1)));
    }
    return ~value;
}
void u16(QByteArray &b, quint16 v) {
    char p[2];
    qToLittleEndian(v, p);
    b.append(p, 2);
}
void u32(QByteArray &b, quint32 v) {
    char p[4];
    qToLittleEndian(v, p);
    b.append(p, 4);
}
quint16 r16(const QByteArray &b, qsizetype at) {
    if (at < 0 || at + 2 > b.size())
        throw std::runtime_error("Truncated stamp archive");
    return qFromLittleEndian<quint16>(b.constData() + at);
}
quint32 r32(const QByteArray &b, qsizetype at) {
    if (at < 0 || at + 4 > b.size())
        throw std::runtime_error("Truncated stamp archive");
    return qFromLittleEndian<quint32>(b.constData() + at);
}
void safeName(const QString &name) {
    auto parts = name.split('/');
    if (parts.size() != 2 || parts[0].isEmpty() || parts[0].contains('\\') ||
        parts[0].contains('.') || parts[1].contains('\\') || parts[1].isEmpty() || parts[1] == "..")
        throw std::runtime_error("Unsafe path in stamp archive");
}
} // namespace
QByteArray writeArchive(const ArchiveEntries &entries) {
    if (entries.size() > 2000)
        throw std::runtime_error("Stamp collection is too large");
    QByteArray body, central;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        safeName(it.key());
        auto name = it.key().toUtf8();
        auto data = it.value();
        auto checksum = crc(data);
        quint32 offset = body.size();
        u32(body, 0x04034b50);
        u16(body, 20);
        u16(body, 0x800);
        u16(body, 0);
        u16(body, 0);
        u16(body, 0x21);
        u32(body, checksum);
        u32(body, data.size());
        u32(body, data.size());
        u16(body, name.size());
        u16(body, 0);
        body += name;
        body += data;
        u32(central, 0x02014b50);
        u16(central, 20);
        u16(central, 20);
        u16(central, 0x800);
        u16(central, 0);
        u16(central, 0);
        u16(central, 0x21);
        u32(central, checksum);
        u32(central, data.size());
        u32(central, data.size());
        u16(central, name.size());
        u16(central, 0);
        u16(central, 0);
        u16(central, 0);
        u16(central, 0);
        u32(central, 0);
        u32(central, offset);
        central += name;
        if (body.size() + central.size() > 100 * 1024 * 1024)
            throw std::runtime_error("Stamp archive exceeds 100 MiB");
    }
    const quint32 offset = body.size();
    body += central;
    u32(body, 0x06054b50);
    u16(body, 0);
    u16(body, 0);
    u16(body, entries.size());
    u16(body, entries.size());
    u32(body, central.size());
    u32(body, offset);
    u16(body, 0);
    return body;
}
ArchiveEntries readArchive(const QByteArray &b) {
    if (b.size() > 100 * 1024 * 1024 || b.size() < 22)
        throw std::runtime_error("Invalid stamp archive size");
    ArchiveEntries result;
    QMap<QString, quint32> offsets;
    QMap<QString, quint32> checksums;
    qsizetype pos = 0;
    while (r32(b, pos) == 0x04034b50) {
        const auto length = r32(b, pos + 18);
        const int nameLength = r16(b, pos + 26), extra = r16(b, pos + 28);
        const auto end = pos + 30 + nameLength + extra + length;
        if (r16(b, pos + 8) != 0 || (r16(b, pos + 6) & ~0x800) || r32(b, pos + 22) != length ||
            nameLength > 128 || end > b.size())
            throw std::runtime_error(
                "Only bounded, uncompressed yolostamps ZIP entries are supported");
        auto name = QString::fromUtf8(b.mid(pos + 30, nameLength));
        safeName(name);
        if (result.contains(name) || result.size() >= 2000)
            throw std::runtime_error("Duplicate or excessive stamp archive entries");
        auto data = b.mid(pos + 30 + nameLength + extra, length);
        const auto checksum = r32(b, pos + 14);
        if (crc(data) != checksum)
            throw std::runtime_error("Damaged stamp archive entry");
        result.insert(name, data);
        offsets.insert(name, pos);
        checksums.insert(name, checksum);
        pos = end;
    }
    const auto central = pos;
    int count = 0;
    QSet<QString> seen;
    while (r32(b, pos) == 0x02014b50) {
        const int n = r16(b, pos + 28), extra = r16(b, pos + 30), comment = r16(b, pos + 32);
        auto name = QString::fromUtf8(b.mid(pos + 46, n));
        if (pos + 46 + n + extra + comment > b.size() || !result.contains(name) ||
            seen.contains(name) || r16(b, pos + 10) != 0 || r32(b, pos + 42) != offsets[name] ||
            r32(b, pos + 16) != checksums[name] ||
            r32(b, pos + 20) != quint32(result[name].size()) ||
            r32(b, pos + 24) != quint32(result[name].size()))
            throw std::runtime_error("Invalid ZIP directory");
        seen.insert(name);
        ++count;
        pos += 46 + n + extra + comment;
    }
    if (r32(b, pos) != 0x06054b50 || r16(b, pos + 4) || r16(b, pos + 6) ||
        r16(b, pos + 8) != count || r16(b, pos + 10) != count ||
        r32(b, pos + 12) != quint32(pos - central) || r32(b, pos + 16) != central ||
        pos + 22 + r16(b, pos + 20) != b.size() || count != result.size())
        throw std::runtime_error("Invalid stamp ZIP end record");
    return result;
}
} // namespace yolo::core
