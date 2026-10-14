// SPDX-License-Identifier: AGPL-3.0-only
#include "FileAccess.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <stdexcept>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace yolo::platform {
namespace {
void copy(QIODevice &input, QIODevice &output) {
    qint64 total = 0;
    while (!input.atEnd()) {
        const auto block = input.read(64 * 1024);
        if (block.isEmpty() && !input.atEnd())
            throw std::runtime_error("PDF read failed");
        total += block.size();
        if (total > 512 * 1024 * 1024)
            throw std::runtime_error("PDF exceeds the 512 MiB prototype limit");
        if (output.write(block) != block.size())
            throw std::runtime_error("PDF copy failed");
    }
}
QString path(const QUrl &url) {
    if (url.isLocalFile())
        return url.toLocalFile();
    if (url.scheme() == "qrc")
        return ":" + url.path();
    if (url.scheme() == "content")
        return url.toString(QUrl::FullyEncoded);
    throw std::runtime_error("Only local files and Android document URIs are supported");
}
} // namespace
QString cachePdf(const QUrl &source) {
    return cacheAsset(source, ".pdf");
}
QString cacheAsset(const QUrl &source, const QString &suffix) {
    if (suffix.contains('/') || suffix.contains('\\'))
        throw std::runtime_error("Invalid file suffix");
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!QDir().mkpath(folder))
        throw std::runtime_error("Cannot create PDF cache directory");
#ifdef Q_OS_ANDROID
    if (source.scheme() == "content") {
        const auto uri = QJniObject::fromString(source.toString(QUrl::FullyEncoded));
        const auto context = QNativeInterface::QAndroidApplication::context();
        QJniObject::callStaticMethod<void>("org/yolopdf/FileAccess", "persistReadPermission",
                                           "(Landroid/content/Context;Ljava/lang/String;)V",
                                           context.object(), uri.object<jstring>());
    }
#endif
    QFile input(path(source));
    if (!input.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open the selected PDF");
    QTemporaryFile output(folder + "/document-XXXXXX" + suffix);
    if (!output.open())
        throw std::runtime_error("Cannot create cached PDF");
    copy(input, output);
    if (!output.flush())
        throw std::runtime_error("Cannot flush cached PDF");
    output.setAutoRemove(false);
    return output.fileName();
}
void saveCachedPdf(const QString &cache, const QUrl &destination) {
#ifdef Q_OS_ANDROID
    if (destination.scheme() == "content") {
        const auto context = QNativeInterface::QAndroidApplication::context();
        const auto uri = QJniObject::fromString(destination.toString(QUrl::FullyEncoded));
        const auto source = QJniObject::fromString(cache);
        const auto result = QJniObject::callStaticObjectMethod(
            "org/yolopdf/FileAccess", "copyToUri",
            "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
            context.object(), uri.object<jstring>(), source.object<jstring>());
        if (!result.isValid())
            throw std::runtime_error("Android document provider could not be reached");
        if (!result.toString().isEmpty())
            throw std::runtime_error(result.toString().toStdString());
        return;
    }
#endif
    QFile input(cache);
    if (!input.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cached PDF is unavailable");
    QSaveFile output(path(destination));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot open PDF save destination");
    copy(input, output);
    if (!output.commit())
        throw std::runtime_error("Cannot commit PDF save");
}
} // namespace yolo::platform
