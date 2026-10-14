// SPDX-License-Identifier: AGPL-3.0-only
#include "core/DropRouter.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest/quicktest.h>
class Setup final : public QObject {
    Q_OBJECT
    yolo::core::DropRouter router;
  public slots:
    void qmlEngineAvailable(QQmlEngine *engine) {
        engine->rootContext()->setContextProperty("dropRouter", &router);
    }
};
QUICK_TEST_MAIN_WITH_SETUP(yolopdf_ui, Setup)
#include "quick_main.moc"
