// SPDX-License-Identifier: AGPL-3.0-only
#include "platform/FontLocator.h"
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

class FontLocatorTest final : public QObject {
    Q_OBJECT
  private slots:
    void exactPostscriptMatchDespiteDesktopFamilyAlias() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("fonts"));
        QVERIFY(QDir(directory.path()).mkdir("cache"));
        const auto original = directory.filePath("fonts/original.ttf");
        QVERIFY(QFile::copy(QString(YOLOPDF_CORPUS_DIR) + "/YoloTest-Regular.ttf", original));
        QVERIFY(QFile::copy(QString(YOLOPDF_SOURCE_DIR) +
                                "/third_party/mupdf/resources/fonts/noto/NotoSans-Regular.otf",
                            directory.filePath("fonts/default.otf")));
        QFile config(directory.filePath("fonts.conf"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        const auto xml = QString("<fontconfig><dir>%1</dir><cachedir>%2</cachedir>"
                                 "<match target=\"pattern\"><edit name=\"family\" "
                                 "mode=\"prepend\" binding=\"strong\"><string>Noto Sans</string>"
                                 "</edit></match></fontconfig>")
                             .arg(directory.filePath("fonts").toHtmlEscaped(),
                                  directory.filePath("cache").toHtmlEscaped())
                             .toUtf8();
        QCOMPARE(config.write(xml), qint64(xml.size()));
        config.close();
        const auto previous = qgetenv("FONTCONFIG_FILE");
        const auto restore = qScopeGuard([previous] {
            if (previous.isNull())
                qunsetenv("FONTCONFIG_FILE");
            else
                qputenv("FONTCONFIG_FILE", previous);
        });
        qputenv("FONTCONFIG_FILE", config.fileName().toUtf8());
        const auto exact = yolo::platform::findExactFont("YoloTest-Regular");
        QCOMPARE(exact.file, original);
        QCOMPARE(exact.faceIndex, 0);
        QCOMPARE(yolo::platform::findExactFont("ABCDEF+YoloTest-Regular").file, original);
        QVERIFY(yolo::platform::findExactFont("YoloTest").file.isEmpty());
        QVERIFY(yolo::platform::findExactFont("YolopdfNonexistentFont").file.isEmpty());
    }
};
QTEST_GUILESS_MAIN(FontLocatorTest)
#include "FontLocatorTest.moc"
