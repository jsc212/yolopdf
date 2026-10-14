// SPDX-License-Identifier: AGPL-3.0-only
#include "core/DocumentWorker.h"
#include "platform/FileAccess.h"
#include <QCoreApplication>
#include <QTextStream>
#include <iostream>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto args = app.arguments();
    if (args.size() != 4)
        return 2;
    try {
        if (args[1] == "save")
            yolo::platform::saveCachedPdf(args[2], QUrl::fromLocalFile(args[3]));
        else if (args[1] == "edit") {
            yolo::core::DocumentWorker worker(args[3]);
            worker.open(QUrl::fromLocalFile(args[2]));
            worker.change(worker.activeId, [](auto &doc) { doc.replaceText(0, {0, "éè", {}}); });
            QTextStream(stdout) << worker.activeId << Qt::endl;
            std::string line;
            std::getline(std::cin, line); // Parent kills us, bypassing destructors.
        } else if (args[1] == "restore") {
            yolo::core::DocumentWorker worker(args[3]);
            auto candidates = worker.recoveries();
            if (candidates.size() != 1)
                return 3;
            worker.restore(candidates.front().toMap().value("id").toString());
            auto &doc = worker.session(worker.activeId).document;
            if (doc->textRuns(0).runs[0].text != "éè")
                return 4;
            doc->undo();
            if (doc->textRuns(0).runs[0].text != "Hello éééé")
                return 5;
        } else
            return 2;
    } catch (const std::exception &e) {
        QTextStream(stderr) << e.what() << Qt::endl;
        return 1;
    }
    return 0;
}
