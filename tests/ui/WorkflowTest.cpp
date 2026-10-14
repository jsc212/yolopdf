// SPDX-License-Identifier: AGPL-3.0-only
#include "app/DocumentController.h"
#include "core/AssetLibrary.h"
#include "core/DropRouter.h"
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <functional>
class WorkflowTest final : public QObject {
    Q_OBJECT
  private slots:
    void dragHighlightAndTextApplyUseTheVisibleSelection() {
        QTemporaryDir dir;
        QQmlApplicationEngine engine;
        auto *provider = new PageImageProvider;
        engine.addImageProvider("pages", provider);
        DocumentController controller(provider);
        yolo::core::AssetLibrary library(dir.filePath("library"));
        yolo::core::DropRouter drops;
        engine.rootContext()->setContextProperty("documentController", &controller);
        engine.rootContext()->setContextProperty("assetLibrary", &library);
        engine.rootContext()->setContextProperty("dropRouter", &drops);
        engine.load(QUrl::fromLocalFile(QString(YOLOPDF_SOURCE_DIR) + "/src/ui/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().empty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        QVERIFY(window);
        controller.openPdf(
            QUrl::fromLocalFile(QString(YOLOPDF_CORPUS_DIR) + "/rectangular-clip.pdf"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.hasDocument() && !controller.busy(), 10000);
        std::function<QQuickItem *(QQuickItem *)> find = [&](auto *item) -> QQuickItem * {
            if (item->objectName() == "pagePointer")
                return item;
            for (auto *child : item->childItems())
                if (auto *found = find(child))
                    return found;
            return nullptr;
        };
        QTRY_VERIFY(find(window->contentItem()) != nullptr);
        auto run = controller.document()["runs"].toList().front().toMap();
        auto point = [&](double x, double y) {
            auto *p = find(window->contentItem());
            return p->mapToScene(QPointF(x * p->width() / 300, y * p->width() / 300)).toPoint();
        };
        const double x = run["x"].toDouble(),
                     y = run["y"].toDouble() + run["height"].toDouble() / 2,
                     width = run["width"].toDouble();
        window->setProperty("tool", "Annotate");
        window->setProperty("annotationMode", "Highlight");
        QTest::qWait(50);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(x + width * .1, y));
        QTest::mouseMove(window, point(x + width * .6, y), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point(x + width * .6, y));
        QTRY_COMPARE(controller.document()["annotations"].toList().size(), 1);
        QVERIFY(controller.document()["annotations"].toList()[0].toMap()["width"].toDouble() <
                width * .9);
        controller.undo();
        QTRY_VERIFY(!controller.busy());
        window->setProperty("tool", "Edit text");
        QTest::qWait(50);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(x + width * .5, y));
        auto *editor = window->findChild<QQuickItem *>("textEditor");
        auto *popup = window->findChild<QObject *>("textEditPopup");
        QVERIFY(editor && popup);
        QTRY_VERIFY(popup->property("opened").toBool());
        editor->setProperty("text", QString("Ω"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!controller.busy());
        QVERIFY(!controller.error().isEmpty());
        QVERIFY(popup->property("opened").toBool());
        QCOMPARE(editor->property("text").toString(), QString("Ω"));
        QVERIFY(!controller.document()["dirty"].toBool());
        editor->setProperty("text", "Hi");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!controller.busy());
        QTRY_VERIFY(!popup->property("opened").toBool());
        QCOMPARE(controller.document()["runs"].toList()[0].toMap()["text"].toString(),
                 QString("Hi"));
        window->setProperty("allowQuit", true);
        window->close();
        delete window;
    }
    void passwordPromptKeepsBatchOpenOrder() {
        PageImageProvider provider;
        DocumentController controller(&provider);
        QSignalSpy password(&controller, &DocumentController::passwordRequired);
        const auto encrypted = QUrl::fromLocalFile(QString(YOLOPDF_CORPUS_DIR) + "/encrypted.pdf"),
                   plain = QUrl::fromLocalFile(QString(YOLOPDF_CORPUS_DIR) + "/basic.pdf");
        controller.dropFiles({encrypted, plain}, false);
        QTRY_COMPARE_WITH_TIMEOUT(password.size(), 1, 10000);
        QCOMPARE(password.front()[0].toUrl(), encrypted);
        controller.providePassword("test-user");
        QTRY_COMPARE_WITH_TIMEOUT(controller.tabs().size(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.tabs()[0].toMap()["title"].toString(), QString("encrypted.pdf"));
        QCOMPARE(controller.document()["source"].toUrl(), plain);
    }
    void importedFontComparisonPreviewDoesNotCommit() {
        PageImageProvider provider;
        DocumentController controller(&provider);
        controller.openPdf(QUrl::fromLocalFile(QString(YOLOPDF_CORPUS_DIR) + "/subset-latin.pdf"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.hasDocument() && !controller.busy(), 10000);
        QSignalSpy ready(&controller, &DocumentController::previewFinished);
        controller.previewText(
            0, "Ω", QUrl::fromLocalFile(QString(YOLOPDF_CORPUS_DIR) + "/YoloTest-Regular.ttf"));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        QVERIFY2(ready.front()[0].toBool(), qPrintable(controller.error()));
        QVERIFY(!controller.comparisonOriginal().isEmpty());
        QVERIFY(!controller.comparisonEdited().isEmpty());
        QVERIFY(!controller.document()["dirty"].toBool());
        QVERIFY(!controller.document()["canUndo"].toBool());
        ready.clear();
        controller.previewText(0, "Ω", QUrl::fromLocalFile("/missing/yolopdf-font.ttf"));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        QVERIFY(!ready.front()[0].toBool());
        QVERIFY(controller.comparisonOriginal().isEmpty());
        QVERIFY(controller.comparisonEdited().isEmpty());
        controller.cancelPreview();
        QVERIFY(controller.comparisonOriginal().isEmpty());
    }
    void realWindowCreatesMarkingsAndExportsWithoutReplacingEditableSource() {
        QTemporaryDir dir;
        QQmlApplicationEngine engine;
        auto *provider = new PageImageProvider;
        engine.addImageProvider("pages", provider);
        DocumentController controller(provider);
        yolo::core::AssetLibrary library(dir.filePath("library"));
        yolo::core::DropRouter drops;
        engine.rootContext()->setContextProperty("documentController", &controller);
        engine.rootContext()->setContextProperty("assetLibrary", &library);
        engine.rootContext()->setContextProperty("dropRouter", &drops);
        engine.load(QUrl::fromLocalFile(QString(YOLOPDF_SOURCE_DIR) + "/src/ui/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().empty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        QVERIFY(window);
        std::function<QQuickItem *(QQuickItem *)> findPointer =
            [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == "pagePointer")
                return item;
            for (auto *child : item->childItems())
                if (auto *found = findPointer(child))
                    return found;
            return nullptr;
        };
        auto source = QUrl::fromLocalFile(QString(YOLOPDF_CORPUS_DIR) + "/basic.pdf");
        controller.openPdf(source);
        QTRY_VERIFY_WITH_TIMEOUT(controller.hasDocument() && !controller.busy(), 10000);
        QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
        QTRY_VERIFY_WITH_TIMEOUT(findPointer(window->contentItem()) != nullptr, 10000);
        auto point = [&](double x, double y) {
            auto *pointer = findPointer(window->contentItem());
            const double scale = pointer->width() / 300;
            return pointer->mapToScene(QPointF(x * scale, y * scale)).toPoint();
        };
        QVERIFY(window->setProperty("tool", "Annotate"));
        QVERIFY(window->setProperty("annotationMode", "Marker"));
        QTest::qWait(50);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(30, 110));
        QTest::mouseMove(window, point(70, 105), 10);
        QTest::mouseMove(window, point(110, 110), 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point(110, 110));
        QTRY_COMPARE_WITH_TIMEOUT(controller.document()["annotations"].toList().size(), 1, 10000);
        QVERIFY(controller.error().isEmpty());
        QVERIFY(window->setProperty("annotationMode", "Strikeout"));
        QTest::qWait(50);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(80, 55));
        QTRY_COMPARE_WITH_TIMEOUT(controller.document()["annotations"].toList().size(), 2, 10000);
        controller.undo();
        QTRY_COMPARE_WITH_TIMEOUT(controller.document()["annotations"].toList().size(), 1, 10000);
        controller.redo();
        QTRY_COMPARE_WITH_TIMEOUT(controller.document()["annotations"].toList().size(), 2, 10000);
        const auto saved = QUrl::fromLocalFile(dir.filePath("editable.pdf"));
        controller.saveAs(saved);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(controller.error().isEmpty());
        auto editable = yolo::engine::openDocument(saved.toLocalFile().toStdString());
        QCOMPARE(editable->annotations(0).size(), std::size_t{2});
        const auto flat = QUrl::fromLocalFile(dir.filePath("flat.pdf"));
        controller.exportFlattened(flat);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
        auto flattened = yolo::engine::openDocument(flat.toLocalFile().toStdString());
        QVERIFY(flattened->annotations(0).empty());
        QCOMPARE(controller.document()["source"].toUrl(), saved);
        QCOMPARE(controller.document()["annotations"].toList().size(), 2);
        controller.exportFlattened(saved);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.error().isEmpty());
        editable = yolo::engine::openDocument(saved.toLocalFile().toStdString());
        QCOMPARE(editable->annotations(0).size(), std::size_t{2});
        window->setProperty("tool", "Select");
        QTest::qWait(50);
        auto imageBefore = controller.document()["images"].toList().front().toMap();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(110, 140));
        QTest::qWait(50);
        QTest::mouseMove(window, point(120, 150), 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point(120, 150));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
        auto imageAfter = controller.document()["images"].toList().front().toMap();
        QVERIFY(std::abs(imageAfter["x"].toDouble() - imageBefore["x"].toDouble() - 10) < 1);
        QVERIFY(std::abs(imageAfter["y"].toDouble() - imageBefore["y"].toDouble() - 10) < 1);
        controller.undo();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        auto undone = controller.document()["images"].toList().front().toMap();
        QCOMPARE(undone["x"], imageBefore["x"]);
        window->setProperty("allowQuit", true);
        window->close();
        delete window;
    }
};
int main(int argc, char **argv) {
    QTemporaryDir data;
    qputenv("XDG_DATA_HOME", data.filePath("data").toUtf8());
    qputenv("XDG_CACHE_HOME", data.filePath("cache").toUtf8());
    QGuiApplication app(argc, argv);
    WorkflowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "WorkflowTest.moc"
