// SPDX-License-Identifier: AGPL-3.0-only
#include "core/DocumentWorker.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <future>
using namespace yolo;
namespace {
QUrl fixture(const char *name) {
    return QUrl::fromLocalFile(QString(YOLOPDF_CORPUS_DIR) + "/" + name);
}
} // namespace
class SessionTest : public QObject {
    Q_OBJECT
  private slots:
    void tabsSaveAndChangedOriginal() {
        QTemporaryDir dir;
        core::DocumentWorker worker(dir.filePath("recovery"));
        auto result = worker.open(fixture("subset-latin.pdf"));
        auto first = worker.activeId;
        worker.change(first, [](auto &d) { d.replaceText(0, {0, "éè", {}}); });
        worker.open(fixture("nested.pdf"));
        auto second = worker.activeId;
        QCOMPARE(worker.snapshot().tabs.size(), 2);
        worker.activate(first);
        QCOMPARE(worker.session(first).document->textRuns(0).runs[0].text, std::string("éè"));
        auto output = QUrl::fromLocalFile(dir.filePath("saved.pdf"));
        worker.save(first, output);
        QVERIFY(!worker.snapshot().active.value("dirty").toBool());
        QFile external(output.toLocalFile());
        QVERIFY(external.open(QIODevice::Append));
        external.write("\n% outside change\n");
        external.close();
        const auto original = core::fingerprint(output);
        QVERIFY_EXCEPTION_THROWN(worker.save(first, output), std::exception);
        QCOMPARE(core::fingerprint(output), original);
        worker.undo(first);
        QVERIFY(worker.snapshot().active.value("dirty").toBool());
        worker.undo(first, true);
        QVERIFY(!worker.snapshot().active.value("dirty").toBool());
        worker.activate(second);
        QCOMPARE(worker.snapshot().active.value("pages").toList().size(), 2);
    }
    void undoRestoresSelectedPageIdentity() {
        QTemporaryDir dir;
        core::DocumentWorker worker(dir.path());
        worker.open(fixture("nested.pdf"));
        auto id = worker.activeId;
        auto pages = worker.session(id).document->inspect().pages;
        worker.setSelection(id,
                            {{"activePage", pages[1].id}, {"pages", QVariantList{pages[1].id}}});
        worker.change(id, [](auto &d) { d.rearrangePages({0}); });
        QCOMPARE(worker.snapshot().active.value("pages").toList().size(), 1);
        worker.undo(id);
        auto result = worker.snapshot();
        QCOMPARE(result.active.value("page").toInt(), 1);
        QCOMPARE(result.active.value("selection").toMap().value("pages").toList().front().toInt(),
                 pages[1].id);
    }
    void crossDocumentMoveUndoRedo() {
        QTemporaryDir dir;
        core::DocumentWorker worker(dir.path());
        worker.open(fixture("nested.pdf"));
        auto a = worker.activeId;
        worker.open(fixture("basic.pdf"));
        auto b = worker.activeId;
        worker.transfer(a, b, {0}, 1, true);
        QCOMPARE(worker.session(a).document->inspect().pages.size(), std::size_t{1});
        QCOMPARE(worker.session(b).document->inspect().pages.size(), std::size_t{2});
        worker.undo(b);
        QCOMPARE(worker.session(a).document->inspect().pages.size(), std::size_t{2});
        QCOMPARE(worker.session(b).document->inspect().pages.size(), std::size_t{1});
        worker.undo(a, true);
        QCOMPARE(worker.session(a).document->inspect().pages.size(), std::size_t{1});
        worker.change(a, [](auto &d) { d.rotatePage(0, 90); });
        QVERIFY_EXCEPTION_THROWN(worker.undo(b), std::exception);
        worker.undo(a);
        worker.undo(b);
        QCOMPARE(worker.session(a).document->inspect().pages.size(), std::size_t{2});
    }
    void annotatedSourceSnapshotsDoNotAddUndoSteps() {
        QTemporaryDir dir;
        core::DocumentWorker worker(dir.filePath("recovery"));
        worker.open(fixture("existing-freetext-pages.pdf"));
        const auto a = worker.activeId;
        worker.open(fixture("basic.pdf"));
        const auto b = worker.activeId;
        worker.transfer(a, b, {0}, 1, true);
        QCOMPARE(worker.session(a).document->history().position, 1);
        worker.undo(a);
        QCOMPARE(worker.session(a).document->inspect().pages.size(), std::size_t{2});
        QCOMPARE(worker.session(b).document->inspect().pages.size(), std::size_t{1});
        worker.undo(b, true);
        QCOMPARE(worker.session(a).document->inspect().pages.size(), std::size_t{1});
    }
    void recoveryRestoresJournalAndProtectsChangedSource() {
        QTemporaryDir dir;
        const auto source = dir.filePath("original.pdf");
        QVERIFY(QFile::copy(fixture("subset-latin.pdf").toLocalFile(), source));
        QString id;
        {
            core::DocumentWorker worker(dir.filePath("recovery"));
            worker.open(QUrl::fromLocalFile(source));
            id = worker.activeId;
            worker.change(id, [](auto &d) { d.replaceText(0, {0, "éè", {}}); });
            worker.persist(worker.session(id), true);
            worker.persist(worker.session(id), true);
        }
        {
            core::DocumentWorker worker(dir.filePath("recovery"));
            QCOMPARE(worker.recoveries().size(), 1);
            auto r = worker.restore(id);
            auto restored = worker.activeId;
            QCOMPARE(worker.session(restored).document->textRuns(0).runs[0].text,
                     std::string("éè"));
            worker.undo(restored);
            QCOMPARE(worker.session(restored).document->textRuns(0).runs[0].text,
                     std::string("Hello éééé"));
            worker.undo(restored, true);
            id = restored;
        }
        QFile file(source);
        QVERIFY(file.open(QIODevice::Append));
        file.write("\n% edited elsewhere\n");
        file.close();
        core::DocumentWorker worker(dir.filePath("recovery"));
        auto r = worker.restore(id);
        QVERIFY(r.active.value("source").toUrl().isEmpty());
        QVERIFY_EXCEPTION_THROWN(worker.restore("../other"), std::exception);
    }
    void printCopiesIncludeSavedHighlightsAndPhysicalPageSizes() {
        auto doc = engine::openDocument(fixture("basic.pdf").toLocalFile().toStdString());
        const auto before = doc->render(0).rgba;
        const auto hit = doc->search("yolopdf").front();
        doc->addAnnotation(0, {engine::AnnotationType::Highlight, hit.bounds, "print test"});
        QVERIFY(doc->render(0).rgba != before);
        QTemporaryDir dir;
        QFile save(dir.filePath("highlight.pdf"));
        QVERIFY(save.open(QIODevice::WriteOnly));
        const auto bytes = doc->pdfBytes();
        save.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        save.close();
        auto reopened = engine::openDocument(save.fileName().toStdString());
        QCOMPARE(reopened->annotations(0).size(), std::size_t{1});
        QCOMPARE(reopened->render(0).rgba, doc->render(0).rgba);
        auto marked = platform::preparePrint(*reopened, "Marked");
        auto printed = engine::openDocument(marked.pdf.toStdString());
        const auto printedPixels = printed->render(0).rgba;
        QVERIFY(printed->annotations(0).empty()); // Appearance is now page content.
        doc->undo();
        auto plain = platform::preparePrint(*doc, "Plain");
        auto plainPrinted = engine::openDocument(plain.pdf.toStdString());
        QVERIFY(printedPixels != plainPrinted->render(0).rgba);
        QCOMPARE(printed->inspect().pages.size(), std::size_t{1});
        QDir(marked.folder).removeRecursively();
        QDir(plain.folder).removeRecursively();
        auto geometry = engine::openDocument(fixture("nested.pdf").toLocalFile().toStdString());
        auto bundle = platform::preparePrint(*geometry, "Mixed sizes");
        auto copy = engine::openDocument(bundle.pdf.toStdString());
        auto expected = geometry->inspect().pages, actual = copy->inspect().pages;
        QCOMPARE(actual.size(), expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            QVERIFY(std::abs(actual[i].visibleBounds.x1 - expected[i].visibleBounds.x1) < 1.1);
            QVERIFY(std::abs(actual[i].visibleBounds.y1 - expected[i].visibleBounds.y1) < 1.1);
        }
        QDir(bundle.folder).removeRecursively();
    }
    void renderJobsUseIndependentContextsAndSurviveDocumentClose() {
        auto doc = engine::openDocument(fixture("basic.pdf").toLocalFile().toStdString());
        const auto full = doc->render(0, 72);
        auto scene = doc->renderSource(0);
        auto left = scene->job(), right = scene->job();
        doc->replaceText(0, {0, "Hi", {}});
        doc.reset();
        scene.reset();
        auto a = std::async(std::launch::async,
                            [job = std::move(left)] { return job->render(72, {0, 0, 150, 200}); });
        auto b = std::async(std::launch::async, [job = std::move(right)] {
            return job->render(72, {150, 0, 300, 200});
        });
        auto x = a.get(), y = b.get();
        QCOMPARE(x.width, 150);
        QCOMPARE(y.height, 200);
        for (int row = 0; row < 200; ++row)
            for (int col = 0; col < 300; ++col)
                for (int c = 0; c < 4; ++c) {
                    auto &tile = col < 150 ? x : y;
                    const int tcol = col < 150 ? col : col - 150;
                    QCOMPARE(tile.rgba[(row * 150 + tcol) * 4 + c],
                             full.rgba[(row * 300 + col) * 4 + c]);
                }
    }
};
QTEST_GUILESS_MAIN(SessionTest)
#include "SessionTest.moc"
