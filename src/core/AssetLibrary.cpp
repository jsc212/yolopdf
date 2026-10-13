// SPDX-License-Identifier: AGPL-3.0-only
#include "AssetLibrary.h"
#include "LibraryArchive.h"
#include "engine/api/Document.h"
#include "platform/FileAccess.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent>
#include <cmath>
namespace yolo::core {
namespace {
void write(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error("Cannot write library asset");
}
QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 100 * 1024 * 1024)
        throw std::runtime_error("Cannot read library asset");
    return f.readAll();
}
QString format(const QByteArray &b) {
    if (b.startsWith("\x89PNG"))
        return "png";
    if (b.startsWith("GIF8"))
        return "gif";
    if (b.startsWith("%PDF-"))
        return "pdf";
    if (b.left(4096).contains("<svg"))
        return "svg";
    throw std::runtime_error("Choose PNG, SVG, GIF or PDF stamp artwork");
}
void size(double width, double height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > 5000 || height > 5000)
        throw std::runtime_error("Stamp dimensions must be 0–5000 mm");
}
QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
} // namespace
AssetLibrary::AssetLibrary(QString root, QObject *parent)
    : QObject(parent),
      root_(root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           : root) {
    QDir().mkpath(root_ + "/stamps");
    QDir().mkpath(root_ + "/fonts");
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
}
QVariantList AssetLibrary::list(const QString &kind) const {
    QVariantList result;
    for (auto id : QDir(root_ + "/" + kind).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QUuid(id).isNull())
            continue;
        try {
            auto obj =
                QJsonDocument::fromJson(read(root_ + "/" + kind + "/" + id + "/" +
                                             (kind == "stamps" ? "stamp.json" : "font.json")))
                    .object();
            const auto filename = obj["artwork"].toString();
            if (filename.contains('/') || filename.contains('\\') || filename.contains(".."))
                continue;
            obj["id"] = id;
            auto map = obj.toVariantMap();
            map["file"] = QUrl::fromLocalFile(root_ + "/" + kind + "/" + id + "/" + filename);
            result.append(map);
        } catch (...) {
        }
    }
    return result;
}
QVariantList AssetLibrary::stamps() const {
    return list("stamps");
}
QVariantList AssetLibrary::fonts() const {
    return list("fonts");
}
QString AssetLibrary::importStampNow(const QUrl &url, const QString &name, double width,
                                     double height, bool aspect, int frame) {
    error_.clear();
    QString temporary, folder, id;
    try {
        size(width, height);
        if (name.trimmed().isEmpty() || name.size() > 256 || frame < 0 || frame > 10000)
            throw std::runtime_error("Choose a stamp name and valid frame");
        auto path = url.isLocalFile() ? url.toLocalFile()
                                      : (temporary = platform::cacheAsset(url, ".artwork"));
        auto data = read(path);
        const auto type = format(data);
        id = newId();
        folder = root_ + "/stamps/" + id;
        if (!QDir().mkpath(folder))
            throw std::runtime_error("Cannot create stamp folder");
        const auto artwork = "artwork." + type;
        write(folder + "/" + artwork, data);
        auto proof = engine::createDocument();
        proof->insertBlankPage(0, std::max(width, 1.0), std::max(height, 1.0));
        engine::StampSpec spec;
        spec.artworkFile = (folder + "/" + artwork).toUtf8().toStdString();
        spec.widthMm = width;
        spec.heightMm = height;
        spec.gifFrame = frame;
        proof->placeStamp(0, spec);
        QJsonObject metadata{{"version", 1},       {"id", id},           {"name", name.trimmed()},
                             {"widthMm", width},   {"heightMm", height}, {"aspectLocked", aspect},
                             {"sizeLocked", true}, {"sourceType", type}, {"gifFrame", frame},
                             {"artwork", artwork}};
        write(folder + "/stamp.json", QJsonDocument(metadata).toJson());
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        if (!folder.isEmpty())
            QDir(folder).removeRecursively();
        id.clear();
    }
    if (!temporary.isEmpty())
        QFile::remove(temporary);
    emit changed();
    return id;
}
QUrl AssetLibrary::importFontNow(const QUrl &url) {
    error_.clear();
    QString temporary, folder;
    QUrl result;
    try {
        auto path =
            url.isLocalFile() ? url.toLocalFile() : (temporary = platform::cacheAsset(url, ".ttf"));
        auto data = read(path);
        if (!data.startsWith(QByteArray::fromHex("00010000")) && !data.startsWith("OTTO") &&
            !data.startsWith("ttcf"))
            throw std::runtime_error("Choose a TrueType, OpenType or collection font");
        const auto id = newId();
        folder = root_ + "/fonts/" + id;
        QDir().mkpath(folder);
        auto file = folder + "/font.ttf";
        write(file, data);
        const int handle = QFontDatabase::addApplicationFont(file);
        if (handle < 0)
            throw std::runtime_error("Font could not be loaded");
        const auto families = QFontDatabase::applicationFontFamilies(handle);
        QFontDatabase::removeApplicationFont(handle);
        write(folder + "/font.json", QJsonDocument(QJsonObject{{"id", id},
                                                               {"name", families.join(", ")},
                                                               {"artwork", "font.ttf"},
                                                               {"faceIndex", 0}})
                                         .toJson());
        result = QUrl::fromLocalFile(file);
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        if (!folder.isEmpty())
            QDir(folder).removeRecursively();
    }
    if (!temporary.isEmpty())
        QFile::remove(temporary);
    emit changed();
    return result;
}
void AssetLibrary::removeNow(const QString &kind, const QString &id) {
    error_.clear();
    if ((kind != "stamps" && kind != "fonts") || QUuid(id).isNull()) {
        error_ = "Invalid library item";
        emit changed();
        return;
    }
    if (!QDir(root_ + "/" + kind + "/" + id).removeRecursively())
        error_ = "Cannot delete library item";
    emit changed();
}
void AssetLibrary::exportStampsNow(const QUrl &url) {
    error_.clear();
    QString cached;
    try {
        ArchiveEntries entries;
        qsizetype total = 0;
        for (auto item : stamps()) {
            auto map = item.toMap();
            const auto id = map["id"].toString();
            const auto folder = root_ + "/stamps/" + id;
            total += QFileInfo(folder + "/stamp.json").size() +
                     QFileInfo(map["file"].toUrl().toLocalFile()).size();
            if (total > 100 * 1024 * 1024)
                throw std::runtime_error("Stamp collection exceeds 100 MiB");
            entries.insert(id + "/stamp.json", read(folder + "/stamp.json"));
            entries.insert(id + "/" + map["artwork"].toString(),
                           read(map["file"].toUrl().toLocalFile()));
        }
        cached = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/stamps-" +
                 newId() + ".yolostamps";
        write(cached, writeArchive(entries));
        platform::saveCachedPdf(cached, url);
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    if (!cached.isEmpty())
        QFile::remove(cached);
    emit changed();
}
void AssetLibrary::importStampsNow(const QUrl &url) {
    error_.clear();
    QString temporary;
    QStringList created;
    try {
        auto path = url.isLocalFile() ? url.toLocalFile()
                                      : (temporary = platform::cacheAsset(url, ".yolostamps"));
        auto entries = readArchive(read(path));
        struct Stamp {
            QJsonObject metadata;
            QByteArray artwork;
        };
        QList<Stamp> validated;
        QSet<QString> consumed;
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it.key().endsWith("/stamp.json")) {
                auto metadata = QJsonDocument::fromJson(it.value()).object();
                auto origin = it.key().section('/', 0, 0);
                if (QUuid(origin).isNull())
                    throw std::runtime_error("Invalid stamp ID");
                auto artwork = metadata["artwork"].toString();
                if (artwork != "artwork.png" && artwork != "artwork.svg" &&
                    artwork != "artwork.gif" && artwork != "artwork.pdf")
                    throw std::runtime_error("Invalid artwork path");
                size(metadata["widthMm"].toDouble(), metadata["heightMm"].toDouble());
                if (metadata["name"].toString().trimmed().isEmpty() ||
                    metadata["name"].toString().size() > 256)
                    throw std::runtime_error("Invalid stamp name");
                auto key = origin + "/" + artwork;
                if (!entries.contains(key) || format(entries[key]) != artwork.section('.', 1))
                    throw std::runtime_error("Missing or invalid artwork");
                consumed.insert(it.key());
                consumed.insert(key);
                validated.append({metadata, entries[key]});
            }
        if (consumed.size() != entries.size())
            throw std::runtime_error("Unexpected files in stamp collection");
        for (auto &stamp : validated) {
            auto id = newId();
            auto folder = root_ + "/stamps/" + id;
            if (!QDir().mkpath(folder))
                throw std::runtime_error("Cannot import stamp folder");
            created.append(folder);
            stamp.metadata["id"] = id;
            write(folder + "/" + stamp.metadata["artwork"].toString(), stamp.artwork);
            write(folder + "/stamp.json", QJsonDocument(stamp.metadata).toJson());
        }
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        for (auto &folder : created)
            QDir(folder).removeRecursively();
    }
    if (!temporary.isEmpty())
        QFile::remove(temporary);
    emit changed();
}
void AssetLibrary::run(std::function<Result()> action) {
    if (busy_)
        return;
    busy_ = true;
    error_.clear();
    emit changed();
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        busy_ = false;
        error_ = result.error;
        emit changed();
        if (!result.id.isEmpty())
            emit stampImported(result.id);
        if (!result.file.isEmpty())
            emit fontImported(result.file);
        emit operationFinished(error_.isEmpty());
    });
    watcher->setFuture(QtConcurrent::run([action = std::move(action)] {
        try {
            return action();
        } catch (const std::exception &e) {
            return Result{QString::fromUtf8(e.what()), {}, {}};
        }
    }));
}
void AssetLibrary::importStamp(const QUrl &url, const QString &name, double width, double height,
                               bool aspect, int frame) {
    run([=, root = root_] {
        AssetLibrary worker(root);
        auto id = worker.importStampNow(url, name, width, height, aspect, frame);
        return Result{worker.error_, id, {}};
    });
}
void AssetLibrary::importFont(const QUrl &url) {
    run([url, root = root_] {
        AssetLibrary worker(root);
        auto file = worker.importFontNow(url);
        return Result{worker.error_, {}, file};
    });
}
void AssetLibrary::remove(const QString &kind, const QString &id) {
    run([kind, id, root = root_] {
        AssetLibrary worker(root);
        worker.removeNow(kind, id);
        return Result{worker.error_, {}, {}};
    });
}
void AssetLibrary::exportStamps(const QUrl &url) {
    run([url, root = root_] {
        AssetLibrary worker(root);
        worker.exportStampsNow(url);
        return Result{worker.error_, {}, {}};
    });
}
void AssetLibrary::importStamps(const QUrl &url) {
    run([url, root = root_] {
        AssetLibrary worker(root);
        worker.importStampsNow(url);
        return Result{worker.error_, {}, {}};
    });
}

} // namespace yolo::core
