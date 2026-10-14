// SPDX-License-Identifier: AGPL-3.0-only
#include "DocumentController.h"
#include "core/AssetLibrary.h"
#include "core/DropRouter.h"
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#ifdef YOLOPDF_DESKTOP_PRINT
#include <QApplication>
#endif
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <functional>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

class FileOpenHandler final : public QObject {
  public:
    DocumentController *controller{};
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::FileOpen)
            return false;
        auto url = static_cast<QFileOpenEvent *>(event)->url();
        if (!controller->busy())
            controller->openPdf(url);
        return true;
    }
};

int main(int argc, char *argv[]) {
#if defined(Q_OS_ANDROID) && defined(YOLOPDF_ANDROID_TEST_HOOKS)
    qInfo() << "Test startup: entering main";
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
#endif
#ifdef YOLOPDF_DESKTOP_PRINT
    QApplication app(argc, argv);
#else
    QGuiApplication app(argc, argv);
#endif
#if defined(Q_OS_ANDROID) && defined(YOLOPDF_ANDROID_TEST_HOOKS)
    qInfo() << "Test startup: GUI initialized";
#endif
    QCoreApplication::setApplicationName("yolopdf");
    QCoreApplication::setOrganizationName("yolopdf");
    QCoreApplication::setApplicationVersion("0.1.0");
    auto *provider = new PageImageProvider;
    DocumentController controller(provider);
#if defined(Q_OS_ANDROID) && defined(YOLOPDF_ANDROID_TEST_HOOKS)
    qInfo() << "Test startup: document controller initialized";
#endif
    yolo::core::AssetLibrary library;
    yolo::core::DropRouter drops;
    FileOpenHandler openHandler;
    openHandler.controller = &controller;
    app.installEventFilter(&openHandler);
    QObject::connect(&controller, &DocumentController::textCopied, &app,
                     [](const QString &text) { QGuiApplication::clipboard()->setText(text); });
    QObject::connect(&controller, &DocumentController::imageCopied, &app, [](const QImage &image) {
#ifdef Q_OS_ANDROID
        const auto file = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                          "/clipboard-" + QUuid::createUuid().toString(QUuid::WithoutBraces) +
                          ".png";
        if (image.save(file, "PNG")) {
            const auto source = QJniObject::fromString(file);
            const auto context = QNativeInterface::QAndroidApplication::context();
            QJniObject::callStaticObjectMethod(
                "org/yolopdf/MobileIntegration", "copyImage",
                "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;", context.object(),
                source.object<jstring>());
            QFile::remove(file);
        }
#else
        QGuiApplication::clipboard()->setImage(image);
#endif
    });
    QQmlApplicationEngine engine;
    engine.addImageProvider("pages", provider);
    engine.rootContext()->setContextProperty("documentController", &controller);
    engine.rootContext()->setContextProperty("assetLibrary", &library);
    engine.rootContext()->setContextProperty("dropRouter", &drops);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("YoloPdf", "Main");
#if defined(Q_OS_ANDROID) && defined(YOLOPDF_ANDROID_TEST_HOOKS)
    qInfo() << "Test startup: QML loaded";
#endif
    // A finite startup check for headless CI; successful QML creation is required.
    auto arguments = app.arguments();
#if defined(Q_OS_ANDROID) && !defined(YOLOPDF_ANDROID_TEST_HOOKS)
    // Android activity arguments come from external intents. File-open/save
    // test hooks are available only in an explicitly enabled test build.
    arguments = {arguments.front()};
#endif
    const bool fixtureSpike = arguments.contains("--fixture-spike");
    const bool smoke = arguments.contains("--smoke-test");
    const int openAt = arguments.indexOf("--open");
    const int captureAt = arguments.indexOf("--capture");
    if (captureAt >= 0 && captureAt + 1 < arguments.size()) {
        const QString file = arguments[captureAt + 1];
        QTimer::singleShot(2500, &app, [&, file] {
            auto *root = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
            if (!root) {
                app.exit(1);
                return;
            }
            app.exit(root->grabWindow().save(file) ? 0 : 1);
        });
    }
    if (smoke) {
        if (engine.rootObjects().isEmpty())
            return 1;
        if (fixtureSpike || openAt >= 0) {
#if defined(Q_OS_ANDROID) && defined(YOLOPDF_ANDROID_TEST_HOOKS)
            QTimer::singleShot(90000, &app, [] { QCoreApplication::exit(1); });
#else
            QTimer::singleShot(20000, &app, [] { QCoreApplication::exit(1); });
#endif
            QObject::connect(&controller, &DocumentController::loaded, &app, [&](bool success) {
                if (!success) {
                    QTextStream(stderr) << controller.error() << '\n';
                    app.exit(1);
                    return;
                }
                auto *ready = new QTimer(&app);
                ready->setInterval(50);
                QObject::connect(ready, &QTimer::timeout, &app, [&, ready] {
                    auto *root = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
                    std::function<QQuickItem *(QQuickItem *)> findImage =
                        [&](QQuickItem *item) -> QQuickItem * {
                        if (item->objectName() == "pageImage")
                            return item;
                        for (auto *child : item->childItems())
                            if (auto *found = findImage(child))
                                return found;
                        return nullptr;
                    };
                    auto *image = root ? findImage(root->contentItem()) : nullptr;
                    if (!image)
                        return;
                    if (image->property("status").toInt() == 3) {
                        QTextStream(stderr)
                            << "Page image failed: " << image->property("source").toUrl().toString()
                            << '\n';
                        app.exit(1);
                        return;
                    }
                    if (image->property("status").toInt() != 1)
                        return;
                    ready->stop();
                    ready->deleteLater();
                    const int saveAt = arguments.indexOf("--save-as");
                    if (saveAt >= 0 && saveAt + 1 < arguments.size()) {
                        QObject::connect(
                            &controller, &DocumentController::saved, &app, [&](bool saved) {
#if defined(Q_OS_ANDROID) && defined(YOLOPDF_ANDROID_TEST_HOOKS)
                                if (saved) {
                                    QFile marker(QStandardPaths::writableLocation(
                                                     QStandardPaths::AppLocalDataLocation) +
                                                 "/stage1-complete");
                                    saved = marker.open(QIODevice::WriteOnly) &&
                                            marker.write("ok\n") == 3 && marker.flush();
                                }
#endif
                                app.exit(saved ? 0 : 1);
                            });
                        const auto destination = arguments[saveAt + 1];
                        controller.saveAs(destination.startsWith("content://")
                                              ? QUrl(destination)
                                              : QUrl::fromLocalFile(destination));
                    } else
                        app.exit(0);
                });
                ready->start();
            });
        } else
            QTimer::singleShot(500, &app, &QCoreApplication::quit);
    }
    if (fixtureSpike)
        controller.openSample();
    else if (openAt >= 0 && openAt + 1 < arguments.size())
        controller.openPdf(QUrl::fromLocalFile(arguments[openAt + 1]));
#ifdef Q_OS_ANDROID
    QTimer intents;
    QObject::connect(&intents, &QTimer::timeout, &app, [&] {
        if (controller.busy())
            return;
        const auto uri = QJniObject::callStaticObjectMethod("org/yolopdf/MobileIntegration",
                                                            "takeDocument", "()Ljava/lang/String;")
                             .toString();
        if (!uri.isEmpty())
            controller.openPdf(QUrl(uri));
    });
    intents.start(250);
#else
    if (openAt < 0 && !fixtureSpike)
        for (int i = 1; i < arguments.size(); ++i) {
            if (arguments[i].startsWith("--")) {
                if (arguments[i] == "--capture")
                    ++i;
                continue;
            }
            controller.openPdf(QUrl::fromLocalFile(QFileInfo(arguments[i]).absoluteFilePath()));
            break;
        }
#endif
    return app.exec();
}
