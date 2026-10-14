// SPDX-License-Identifier: AGPL-3.0-only
// Opt-in real-document audit. Inputs and outputs remain outside the checkout.
#include "app/DocumentController.h"
#include "core/AssetLibrary.h"
#include "core/DropRouter.h"
#include "platform/Print.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

using namespace yolo;
using namespace yolo::engine;
namespace {
void require(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
void write(const QString &path, const std::vector<std::uint8_t> &bytes) {
    QFile out(path);
    require(out.open(QIODevice::WriteOnly), "Cannot create audit output");
    require(out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) ==
                qint64(bytes.size()),
            "Incomplete audit output");
}
QByteArray hash(const QString &path) {
    QFile f(path);
    require(f.open(QIODevice::ReadOnly), "Cannot read audit source");
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}
void wait(const std::function<bool()> &ready) {
    QElapsedTimer time;
    time.start();
    while (!ready() && time.elapsed() < 30000)
        QTest::qWait(5);
    require(ready(), "Desktop operation timed out");
}
EditableLine line(IDocument &doc) {
    for (const auto &l : doc.textLines(0))
        if (l.status == TextStatus::OriginalFont && QString::fromStdString(l.text).size() > 4 &&
            l.bounds.x1 - l.bounds.x0 > 30)
            return l;
    throw std::runtime_error("No supported text line available");
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc != 3) {
        qCritical("Usage: document-audit input.pdf output-directory");
        return 2;
    }
    const QString source = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const QString folder = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    require(!folder.startsWith(QString(YOLOPDF_SOURCE_DIR) + "/"),
            "Keep private audit outputs outside the checkout");
    QDir().mkpath(folder);
    const auto originalHash = hash(source);
    const QString assets = QString(YOLOPDF_CORPUS_DIR);
    const auto font = QString(YOLOPDF_SOURCE_DIR) +
                      "/third_party/mupdf/resources/fonts/noto/NotoSans-Regular.otf";
    QJsonArray results;
    int failures = 0;
    auto test = [&](const QString &name, const std::function<void(IDocument &)> &action) {
        QJsonObject result{{"feature", name}};
        try {
            auto doc = openDocument(source.toStdString());
            action(*doc);
            const auto output = folder + "/" + name + ".pdf";
            write(output, doc->pdfBytes());
            auto reopened = openDocument(output.toStdString());
            require(reopened->inspect().pages.size() == doc->inspect().pages.size(),
                    "Saved page count differs");
            for (int p = 0; p < int(doc->inspect().pages.size()); ++p)
                require(reopened->render(p, 48).rgba == doc->render(p, 48).rgba,
                        "Saved appearance differs");
            result.insert("status", "pass");
        } catch (const std::exception &e) {
            ++failures;
            result.insert("status", "fail");
            result.insert("reason", QString::fromUtf8(e.what()));
        }
        results.append(result);
        qInfo().noquote() << name << result["status"].toString() << result["reason"].toString();
    };
    auto firstLine = [&](IDocument &d) { return line(d); };
    test("view-pages-tiles", [&](auto &d) {
        for (int p = 0; p < int(d.inspect().pages.size()); ++p) {
            auto r = d.render(p, 72);
            require(r.width > 0 && r.height > 0, "Empty page render");
            auto scene = d.renderSource(p);
            require(scene->job()->render(72).rgba == r.rgba, "Worker render differs");
            auto tile = scene->job()->render(144, {0, 0, 100, 100});
            require(tile.width > 0, "Empty tile");
        }
    });
    test("search-copy-text", [&](auto &d) {
        auto l = firstLine(d);
        auto q = QString::fromStdString(l.text).left(4).toUtf8().toStdString();
        require(!d.search(q).empty(), "Search cannot find visible text");
        require(!d.copySelection(0, l.bounds.x0, (l.bounds.y0 + l.bounds.y1) / 2, l.bounds.x1,
                                 (l.bounds.y0 + l.bounds.y1) / 2)
                     .empty(),
                "Text selection is empty");
    });
    test("edit-text-preview-undo-redo", [&](auto &d) {
        auto l = firstLine(d);
        auto before = d.render(0).rgba;
        auto text = QString::fromStdString(l.text).chopped(1).toUtf8().toStdString();
        TextReplacement edit{l.id, text};
        auto preview = d.previewLine(0, edit);
        require(preview.width > 0, "Empty text preview");
        require(d.render(0).rgba == before && !d.canUndo(), "Preview committed changes");
        d.replaceLine(0, edit);
        require(d.render(0).rgba != before, "Edit did not alter text appearance");
        d.undo();
        require(d.render(0).rgba == before, "Text undo changed original appearance");
        d.redo();
    });
    test("edit-text-colour-size", [&](auto &d) {
        auto l = firstLine(d);
        TextReplacement e{l.id, l.text};
        e.changeColor = true;
        e.color = {0, 0, 1};
        d.replaceLine(0, e);
    });
    test("add-text", [&](auto &d) {
        AddTextSpec s;
        s.text = "Audit";
        s.fontFile = font.toStdString();
        s.x = 40;
        s.y = 40;
        s.fontSize = 12;
        d.addText(0, s);
        require(!d.search("Audit").empty(), "Added text is not searchable");
    });
    for (auto pair : {std::pair{"highlight", AnnotationType::Highlight},
                      {"strikeout", AnnotationType::Strikeout},
                      {"note", AnnotationType::Note},
                      {"arrow", AnnotationType::Arrow},
                      {"rectangle", AnnotationType::Rectangle},
                      {"marker", AnnotationType::Marker}})
        test(pair.first, [&, type = pair.second](auto &d) {
            auto l = firstLine(d);
            AnnotationSpec s;
            s.type = type;
            s.bounds = l.bounds;
            s.contents = "Audit note";
            s.points = {{40, 40}, {70, 50}, {100, 40}};
            s.opacity = .4;
            auto n = d.annotations(0).size();
            auto before = d.render(0).rgba;
            d.addAnnotation(0, s);
            require(d.annotations(0).size() == n + 1, "Annotation missing");
            require(d.render(0).rgba != before, "Annotation invisible");
            auto id = d.annotations(0).back().id;
            d.setAnnotationOpacity(0, id, .6);
            d.undo();
            d.undo();
            require(d.render(0).rgba == before, "Annotation undo changed original");
            d.redo();
            d.redo();
        });
    test("annotation-move-delete", [&](auto &d) {
        AnnotationSpec s;
        s.type = AnnotationType::Rectangle;
        s.bounds = {30, 30, 80, 70};
        d.addAnnotation(0, s);
        auto n = d.annotations(0).size();
        auto id = d.annotations(0).back().id;
        d.moveAnnotation(0, id, {40, 40, 90, 80});
        d.deleteAnnotation(0, id);
        require(d.annotations(0).size() == n - 1, "Annotation delete failed");
        d.undo();
    });
    test("original-image-inventory", [&](auto &d) {
        int count = 0;
        for (int p = 0; p < int(d.inspect().pages.size()); ++p)
            count += d.images(p).size();
        results.append(QJsonObject{
            {"feature", "existing-image-selection"},
            {"status", count ? "available" : "not-applicable"},
            {"count", count},
            {"note", count ? "" : "Vector artwork is not an image object; use area copy"}});
    });
    test("existing-text-annotations", [&](auto &d) {
        int n = 0;
        for (int p = 0; p < int(d.inspect().pages.size()); ++p)
            for (auto a : d.annotations(p))
                if (a.type == AnnotationType::FreeText)
                    ++n;
        if (n)
            results.append(QJsonObject{{"feature", "existing-annotation-text-editing"},
                                       {"status", "unsupported"},
                                       {"count", n},
                                       {"reason", "Font-preserving editing of existing text "
                                                  "annotation appearances is not implemented"}});
    });
    test("font-import-preview", [&](auto &d) {
        auto l = firstLine(d);
        auto before = d.render(0).rgba;
        TextReplacement e{l.id, "Ω", font.toStdString()};
        auto preview = d.previewLine(0, e);
        require(preview.width > 0, "Imported font preview failed");
        require(d.render(0).rgba == before && !d.canUndo(), "Imported font preview committed");
        d.replaceLine(0, e);
        require(!d.search("Ω").empty(), "Imported font edit not searchable");
    });
    test("text-size", [&](auto &d) {
        auto l = firstLine(d);
        auto &style = l.segments.front().style;
        const auto &m = style.ctm, &tm = style.textMatrix;
        double size =
            style.fontSize * std::hypot(tm[2] * m[0] + tm[3] * m[2], tm[2] * m[1] + tm[3] * m[3]);
        TextReplacement e{l.id, l.text};
        e.fontSize = size * .8;
        d.replaceLine(0, e);
    });
    test("overflow-protection", [&](auto &d) {
        auto l = firstLine(d);
        auto before = d.render(0).rgba;
        TextReplacement e{l.id, l.text + l.text + l.text};
        bool rejected = false;
        try {
            d.replaceLine(0, e);
        } catch (const Error &error) {
            rejected = error.code() == ErrorCode::Overflow || error.code() == ErrorCode::UnsafeEdit;
        }
        require(rejected, "Overflow silently allowed");
        require(d.render(0).rgba == before && !d.canUndo(), "Rejected edit altered original");
    });
    test("stamp-font-library-roundtrip", [&](auto &d) {
        core::AssetLibrary library(folder + "/assets");
        library.importFont(QUrl::fromLocalFile(font));
        wait([&] { return !library.busy(); });
        require(library.error().isEmpty(), "Font library import failed");
        library.importStamp(QUrl::fromLocalFile(assets + "/vector.svg"), "Audit", 40, 20);
        wait([&] { return !library.busy(); });
        require(library.error().isEmpty(), "Stamp library import failed");
        library.exportStamps(QUrl::fromLocalFile(folder + "/stamps.yolostamps"));
        wait([&] { return !library.busy(); });
        require(library.error().isEmpty(), "Stamp collection export failed");
        core::AssetLibrary imported(folder + "/imported-assets");
        imported.importStamps(QUrl::fromLocalFile(folder + "/stamps.yolostamps"));
        wait([&] { return !imported.busy(); });
        require(imported.error().isEmpty(), "Stamp collection import failed");
        auto stamp = imported.stamps().front().toMap();
        d.placeStamp(0,
                     {stamp["file"].toUrl().toLocalFile().toStdString(), "audit", 10, 10, 40, 20});
    });
    test("area-copy", [&](auto &d) {
        auto r = d.copyArea(0, {20, 20, 150, 100}, 150);
        require(r.width > 0 && r.height > 0, "Empty area copy");
        require(core::image(r).save(folder + "/area.png"), "Area export failed");
    });
    test("images-insert-copy-transform-replace-delete", [&](auto &d) {
        auto before = d.images(0).size();
        d.insertImage(0, (assets + "/red-alpha.png").toStdString(), {40, 0, 0, 40, 40, 40});
        auto image = d.images(0).back();
        require(d.images(0).size() == before + 1, "Image insertion failed");
        require(core::image(d.copyImage(0, image.id)).save(folder + "/image.png"),
                "Image copy/export failed");
        auto target = createDocument();
        target->insertBlankPage(0, 210, 297);
        d.copyImageTo(0, image.id, *target, 0, {40, 0, 0, 40, 20, 20});
        require(target->images(0).size() == 1, "Cross-tab image paste failed");
        d.editImage(0, {image.id, ImageAction::Transform, {0, 40, -40, 0, 90, 40}, {}});
        d.editImage(0, {image.id, ImageAction::Replace, {}, (assets + "/blue.png").toStdString()});
        d.editImage(0, {image.id, ImageAction::Delete, {}, {}});
        require(d.images(0).size() == before, "Image deletion affected wrong occurrence");
        d.undo();
    });
    for (const char *art : {"red-alpha.png", "vector.svg", "frames.gif", "basic.pdf"})
        test("stamp-" + QString(art).section('.', -1), [&, art](auto &d) {
            StampSpec s{(assets + "/" + art).toStdString(), "audit", 10, 10, 40, 20, 15, .5};
            d.placeStamp(0, s);
            auto id = d.annotations(0).back().id;
            d.rotateStamp(0, id, 90);
            d.duplicateStamp(0, id, 50, 0);
            auto box = d.annotations(0).front().bounds;
            for (auto a : d.annotations(0))
                if (a.id == id)
                    box = a.bounds;
            d.moveAnnotation(0, id, {box.x0 + 10, box.y0 + 10, box.x1 + 10, box.y1 + 10});
            d.setAnnotationOpacity(0, id, .6);
        });
    test("flatten-stamps", [&](auto &d) {
        d.placeStamp(0, {(assets + "/vector.svg").toStdString(), "audit", 10, 10, 40, 20});
        d.flattenStamps();
        for (auto a : d.annotations(0))
            require(a.type != AnnotationType::Stamp, "Stamp not flattened");
    });
    test("flattened-copy-print", [&](auto &d) {
        AnnotationSpec s;
        s.type = AnnotationType::Marker;
        s.points = {{40, 40}, {100, 50}};
        s.opacity = .4;
        d.addAnnotation(0, s);
        auto before = d.render(0).rgba;
        auto history = d.history().position;
        write(folder + "/flattened.pdf", d.flattenedBytes());
        require(d.render(0).rgba == before && d.history().position == history,
                "Flattened export changed editable original");
        auto flat = openDocument((folder + "/flattened.pdf").toStdString());
        require(flat->annotations(0).empty(), "Flattened copy contains editable markings");
        auto bundle = platform::preparePrint(d, "Audit");
        QFile::remove(folder + "/print.pdf");
        require(QFile::copy(bundle.pdf, folder + "/print.pdf"), "Print output copy failed");
        QDir(bundle.folder).removeRecursively();
    });
    test("pages-insert-delete-reorder", [&](auto &d) {
        int n = d.inspect().pages.size();
        d.insertPages(1, {{(assets + "/basic.pdf").toStdString(), {0}}});
        require(d.inspect().pages.size() == n + 1, "Page insertion failed");
        std::vector<int> order(n + 1);
        std::iota(order.begin(), order.end(), 0);
        order.erase(order.begin() + 1);
        d.rearrangePages(order);
        require(d.inspect().pages.size() == n, "Page deletion failed");
        order.resize(n);
        std::iota(order.begin(), order.end(), 0);
        std::reverse(order.begin(), order.end());
        d.rearrangePages(order);
    });
    test("pages-duplicate", [&](auto &d) {
        std::vector<int> order(d.inspect().pages.size());
        std::iota(order.begin(), order.end(), 0);
        order.push_back(0);
        try {
            d.rearrangePages(order);
        } catch (const Error &e) {
            if (e.code() != ErrorCode::Unsupported)
                throw;
            results.append(QJsonObject{{"feature", "tagged-page-duplication"},
                                       {"status", "unsupported"},
                                       {"reason", QString::fromUtf8(e.what())}});
        }
    });
    test("pages-rotate-blank-extract", [&](auto &d) {
        auto r = d.render(0);
        d.rotatePage(0, 90);
        auto turned = d.render(0);
        require(turned.width == r.height && turned.height == r.width,
                "Rotated page size incorrect");
        d.insertBlankPage(1, 210, 297);
        auto extracted = createDocument();
        extracted->insertPages(0, {{source.toStdString(), {0}}});
        write(folder + "/extracted.pdf", extracted->pdfBytes());
    });
    test("recovery-native-history", [&](auto &d) {
        auto originalCount = d.annotations(0).size();
        AnnotationSpec s;
        s.type = AnnotationType::Rectangle;
        s.bounds = {30, 30, 80, 60};
        d.addAnnotation(0, s);
        auto data = d.recoveryData();
        write(folder + "/snapshot.pdf", data.pdf);
        auto restored = openDocument((folder + "/snapshot.pdf").toStdString());
        restored->loadRecoveryJournal(data.journal);
        require(restored->annotations(0).size() == originalCount + 1, "Recovery lost edit");
        restored->undo();
        if (restored->annotations(0).size() != originalCount) {
            auto h = restored->history();
            for (auto label : h.labels)
                qInfo() << "recovery label" << QString::fromStdString(label);
            throw std::runtime_error("Recovery undo leaves " +
                                     std::to_string(restored->annotations(0).size()) +
                                     " annotations; expected " + std::to_string(originalCount));
        };
    });
    test("core-safe-save-cross-tab-move", [&](auto &) {
        core::DocumentWorker w(folder + "/recovery");
        auto first = w.open(QUrl::fromLocalFile(source));
        auto a = w.activeId;
        w.open(QUrl::fromLocalFile(assets + "/basic.pdf"));
        auto b = w.activeId;
        auto n = w.session(a).document->inspect().pages.size();
        w.transfer(a, b, {0}, 1, true);
        require(w.session(a).document->inspect().pages.size() == n - 1, "Cross-tab move failed");
        w.undo(a);
        auto undoCount = w.session(a).document->inspect().pages.size();
        if (undoCount != n)
            throw std::runtime_error("Composite undo restored " + std::to_string(undoCount) +
                                     " pages instead of " + std::to_string(n));
        w.save(a, QUrl::fromLocalFile(folder + "/safe-save.pdf"));
    });
    test("desktop-text-highlight-marker", [&](auto &) {
        QQmlApplicationEngine qml;
        auto *provider = new PageImageProvider;
        qml.addImageProvider("pages", provider);
        DocumentController controller(provider);
        core::AssetLibrary library(folder + "/library");
        core::DropRouter drops;
        qml.rootContext()->setContextProperty("documentController", &controller);
        qml.rootContext()->setContextProperty("assetLibrary", &library);
        qml.rootContext()->setContextProperty("dropRouter", &drops);
        qml.load(QUrl::fromLocalFile(QString(YOLOPDF_SOURCE_DIR) + "/src/ui/qml/Main.qml"));
        require(!qml.rootObjects().empty(), "Desktop QML failed");
        auto *window = qobject_cast<QQuickWindow *>(qml.rootObjects().front());
        require(window, "Missing desktop window");
        auto cleanup = qScopeGuard([window] {
            window->setProperty("allowQuit", true);
            delete window;
        });
        controller.openPdf(QUrl::fromLocalFile(source));
        wait([&] { return controller.hasDocument() && !controller.busy(); });
        require(controller.error().isEmpty(), "Desktop open failed");
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == "pagePointer")
                return item;
            for (auto *child : item->childItems())
                if (auto *found = find(child))
                    return found;
            return nullptr;
        };
        wait([&] { return find(window->contentItem()) != nullptr; });
        QTest::qWait(100);
        auto lines = controller.document()["runs"].toList();
        QVariantMap chosen;
        for (const auto &l : lines) {
            auto m = l.toMap();
            if (m["status"] == "Original" && m["width"].toDouble() > 40 &&
                m["text"].toString().size() > 4) {
                chosen = m;
                break;
            }
        }
        require(!chosen.empty(), "Desktop offers no editable text");
        auto point = [&](double x, double y) {
            auto *p = find(window->contentItem());
            auto pg = controller.document()["pages"].toList()[0].toMap();
            double scale = p->width() / pg["width"].toDouble();
            return p->mapToScene({x * scale, y * scale}).toPoint();
        };
        const double x = chosen["x"].toDouble(),
                     y = chosen["y"].toDouble() + chosen["height"].toDouble() / 2,
                     w = chosen["width"].toDouble();
        const int annotationCount = controller.document()["annotations"].toList().size();
        window->setProperty("tool", "Annotate");
        window->setProperty("annotationMode", "Highlight");
        QTest::qWait(30);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(x + w * .1, y));
        QTest::mouseMove(window, point(x + w * .6, y + 1), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point(x + w * .6, y + 1));
        wait([&] { return !controller.busy(); });
        require(controller.document()["annotations"].toList().size() == annotationCount + 1,
                "Dragging over text did not highlight it");
        for (auto a : controller.document()["annotations"].toList())
            if (a.toMap()["type"] == "Highlight")
                require(a.toMap()["width"].toDouble() < w * .9,
                        "Dragging marked the whole line instead of selected words");
        controller.undo();
        wait([&] { return !controller.busy(); });
        window->setProperty("tool", "Edit text");
        QTest::qWait(30);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(x + w * .5, y));
        auto *editor = window->findChild<QQuickItem *>("textEditor");
        require(editor && editor->isVisible(), "Clicking editable text did not open the editor");
        editor->setProperty("text", chosen["text"].toString().chopped(1));
        QTest::keyClick(window, Qt::Key_Return);
        wait([&] { return !controller.busy(); });
        require(controller.document()["dirty"].toBool(), "Enter did not apply text edit");
        controller.saveAs(QUrl::fromLocalFile(folder + "/desktop-edited.pdf"));
        wait([&] { return !controller.busy(); });
        require(controller.error().isEmpty(), "Desktop save failed");
        window->setProperty("tool", "Annotate");
        window->setProperty("annotationMode", "Marker");
        QTest::qWait(50);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(40, 40));
        QTest::mouseMove(window, point(80, 45), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point(80, 45));
        wait([&] { return !controller.busy(); });
        require(controller.error().isEmpty(), "Desktop marker failed");
        controller.exportFlattened(QUrl::fromLocalFile(folder + "/desktop-flattened.pdf"));
        wait([&] { return !controller.busy(); });
        require(controller.error().isEmpty(), "Desktop flattened export failed");
        window->setProperty("tool", "Select");
        QTest::qWait(50);
        QSignalSpy copied(&controller, &DocumentController::textCopied);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(x + w * .1, y));
        QTest::mouseMove(window, point(x + w * .5, y), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point(x + w * .5, y));
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        wait([&] { return !copied.empty(); });
        require(!copied.front()[0].toString().isEmpty(), "Desktop text copy is empty");
        controller.saveAs(QUrl::fromLocalFile(folder + "/desktop-marked.pdf"));
        wait([&] { return !controller.busy(); });
        require(controller.error().isEmpty(), "Desktop marked save failed");
    });
    require(hash(source) == originalHash, "Audit changed original source");
    QFile report(folder + "/audit.json");
    report.open(QIODevice::WriteOnly);
    report.write(
        QJsonDocument(
            QJsonObject{{"results", results}, {"failures", failures}, {"sourceUnchanged", true}})
            .toJson());
    return failures ? 1 : 0;
}
