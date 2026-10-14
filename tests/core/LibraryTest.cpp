// SPDX-License-Identifier: AGPL-3.0-only
#include "core/AssetLibrary.h"
#include "core/LibraryArchive.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace yolo::core;
namespace {
QUrl fixture(const char *name) {
    return QUrl::fromLocalFile(QString(YOLOPDF_CORPUS_DIR) + "/" + name);
}
void write(const QString &path, const QByteArray &data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        throw std::runtime_error("Test write failed");
}
} // namespace
class LibraryTest final : public QObject {
    Q_OBJECT
  private slots:
    void asyncLibraryRoundTripAndInvalidArchiveRollback() {
        QTemporaryDir dir;
        AssetLibrary library(dir.filePath("a"));
        QSignalSpy complete(&library, &AssetLibrary::operationFinished);
        library.importStamp(fixture("vector.svg"), "Vector seal", 40, 20, true, 0);
        QTRY_COMPARE_WITH_TIMEOUT(complete.size(), 1, 10000);
        QVERIFY(complete.back()[0].toBool());
        auto stamps = library.stamps();
        QCOMPARE(stamps.size(), 1);
        auto first = stamps.front().toMap();
        QCOMPARE(first["widthMm"].toDouble(), 40.0);
        const auto collection = dir.filePath("stamps.yolostamps");
        library.exportStamps(QUrl::fromLocalFile(collection));
        QTRY_COMPARE_WITH_TIMEOUT(complete.size(), 2, 10000);
        QVERIFY(complete.back()[0].toBool());
        QFile file(collection);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto archive = file.readAll();
        file.close();
        auto entries = readArchive(archive);
        QCOMPARE(entries.size(), 2);
        AssetLibrary second(dir.filePath("b"));
        QSignalSpy done(&second, &AssetLibrary::operationFinished);
        second.importStamps(QUrl::fromLocalFile(collection));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(done.back()[0].toBool());
        auto imported = second.stamps().front().toMap();
        QCOMPARE(imported["name"], first["name"]);
        QVERIFY(imported["id"] != first["id"]);
        // A valid ZIP with invalid metadata must leave an existing library intact.
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it.key().endsWith("stamp.json")) {
                auto obj = QJsonDocument::fromJson(it.value()).object();
                obj["widthMm"] = -1;
                it.value() = QJsonDocument(obj).toJson();
            }
        const auto broken = dir.filePath("broken.yolostamps");
        write(broken, writeArchive(entries));
        second.importStamps(QUrl::fromLocalFile(broken));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 10000);
        QVERIFY(!done.back()[0].toBool());
        QCOMPARE(second.stamps().size(), 1);
        auto corrupt = archive;
        corrupt[40] = char(corrupt[40] ^ 0x55);
        QVERIFY_EXCEPTION_THROWN(readArchive(corrupt), std::exception);
        QVERIFY_EXCEPTION_THROWN(writeArchive({{"../outside", "bad"}}), std::exception);
        second.remove("stamps", imported["id"].toString());
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 3, 10000);
        QVERIFY(second.stamps().empty());
        QCOMPARE(library.stamps().size(), 1);
    }
    void fontsPersistAndRejectInvalidImport() {
        QTemporaryDir dir;
        AssetLibrary library(dir.path());
        QSignalSpy done(&library, &AssetLibrary::operationFinished);
        library.importFont(fixture("YoloTest-Regular.ttf"));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(done.back()[0].toBool());
        auto fonts = library.fonts();
        QCOMPARE(fonts.size(), 1);
        auto stored = fonts.front().toMap()["file"].toUrl().toLocalFile();
        QVERIFY(QFile::exists(stored));
        library.importFont(fixture("basic.pdf"));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 10000);
        QVERIFY(!done.back()[0].toBool());
        QCOMPARE(library.fonts().size(), 1);
        AssetLibrary reopened(dir.path());
        QCOMPARE(reopened.fonts().size(), 1);
    }
};
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    LibraryTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "LibraryTest.moc"
