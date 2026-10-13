// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/api/Document.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace yolo::engine;
namespace {
std::string fixture(const char *name) {
    return std::string(YOLOPDF_CORPUS_DIR) + "/" + name;
}
void save(const QString &path, IDocument &doc) {
    const auto bytes = doc.pdfBytes();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(reinterpret_cast<const char *>(bytes.data()),
                                                       bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Test output write failed");
}
bool withinOneLevel(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(int(a[i]) - int(b[i])) > 1)
            return false;
    return true;
}
} // namespace
class Stage1Test : public QObject {
    Q_OBJECT
  private slots:
    void replacementUndoRedoAndSerialization() {
        auto doc = openDocument(fixture("basic.pdf"));
        const auto before = doc->render(0);
        doc->replaceText(0, {0, "Hi yolopdf", {}});
        QCOMPARE(doc->textRuns(0).runs[0].text, std::string("Hi yolopdf"));
        QVERIFY(doc->canUndo());
        QTemporaryDir dir;
        const auto output = dir.filePath("edited.pdf");
        save(output, *doc);
        auto reopened = openDocument(output.toStdString());
        QCOMPARE(reopened->textRuns(0).runs[0].text, std::string("Hi yolopdf"));
        doc->undo();
        QCOMPARE(doc->render(0).rgba, before.rgba);
        QVERIFY(doc->canRedo());
        doc->redo();
        QCOMPARE(doc->textRuns(0).runs[0].text, std::string("Hi yolopdf"));
    }
    void sharedFormCopyOnWrite() {
        auto doc = openDocument(fixture("nested.pdf"));
        const auto other = doc->render(1);
        doc->replaceText(0, {0, "Text", {}});
        QCOMPARE(doc->render(1).rgba, other.rgba);
        QCOMPARE(doc->textRuns(1).runs[0].text, std::string("Shared text"));
        doc->undo();
        QCOMPARE(doc->textRuns(0).runs[0].text, std::string("Shared text"));
    }
    void positionedTextKeepsNeighbour() {
        auto doc = openDocument(fixture("positioned-text.pdf"));
        const auto before = doc->textRuns(0);
        doc->replaceText(0, {0, "Hi yolopdf", {}});
        const auto after = doc->textRuns(0);
        QCOMPARE(after.runs[1].text, before.runs[1].text);
        QCOMPARE(after.runs[1].textMatrix[4], before.runs[1].textMatrix[4]);
        doc->replaceText(0, {2, "One", {}});
        doc->replaceText(0, {3, "Two", {}});
        QCOMPARE(doc->textRuns(0).runs[3].text, std::string("Two"));
    }
    void subsetAccentsAndCjk() {
        auto latin = openDocument(fixture("subset-latin.pdf"));
        latin->replaceText(0, {0, "éè", {}});
        QCOMPARE(latin->textRuns(0).runs[0].text, std::string("éè"));
        auto cjk = openDocument(fixture("subset-cjk.pdf"));
        cjk->replaceText(0, {0, "日文", {}});
        QCOMPARE(cjk->textRuns(0).runs[0].text, std::string("日文"));
    }
    void missingGlyphAndOverflowRollBack() {
        auto doc = openDocument(fixture("subset-latin.pdf"));
        const auto before = doc->render(0);
        try {
            doc->replaceText(0, {0, "Ω", {}});
            QFAIL("Missing glyph unexpectedly accepted");
        } catch (const Error &error) {
            QVERIFY(error.code() == ErrorCode::NeedsFont);
        }
        QVERIFY(!doc->canUndo());
        QCOMPARE(doc->render(0).rgba, before.rgba);
        try {
            doc->replaceText(0, {0, std::string(100, 'A'), fixture("YoloTest-Regular.ttf")});
            QFAIL("Overflow unexpectedly accepted");
        } catch (const Error &error) {
            QVERIFY(error.code() == ErrorCode::Overflow);
        }
        QVERIFY(!doc->canUndo());
        QCOMPARE(doc->render(0).rgba, before.rgba);
        doc->replaceText(0, {0, "Ω", fixture("YoloTest-Regular.ttf")});
        QCOMPARE(doc->textRuns(0).runs[0].text, std::string("Ω"));
    }
    void collectionFaceAndGraphicsStateFont() {
        auto doc = openDocument(fixture("subset-latin.pdf"));
        doc->replaceText(0, {0, "Ω", fixture("two-faces.ttc"), 1});
        QCOMPARE(doc->textRuns(0).runs[0].fontName, std::string("YoloAlternate Regular"));
        QTemporaryDir dir;
        save(dir.filePath("collection.pdf"), *doc);
        auto reopened = openDocument(dir.filePath("collection.pdf").toStdString());
        QCOMPARE(reopened->textRuns(0).runs[0].text, std::string("Ω"));
        auto gs = openDocument(fixture("graphics-state-font.pdf"));
        const auto before = gs->textRuns(0).runs[1];
        gs->replaceText(0, {0, "Ω", fixture("YoloTest-Regular.ttf")});
        const auto after = gs->textRuns(0).runs[1];
        QCOMPARE(after.fontName, before.fontName);
        for (std::size_t i = 0; i < after.textMatrix.size(); ++i)
            QVERIFY(std::abs(after.textMatrix[i] - before.textMatrix[i]) < .0001);
    }
    void unsupportedAndMarkedContent() {
        for (const char *file : {"type3.pdf", "actual-text.pdf", "unsupported-modes.pdf",
                                 "vertical.pdf", "complex-script.pdf"}) {
            auto doc = openDocument(fixture(file));
            const auto text = doc->textRuns(0);
            QVERIFY(!text.runs.empty());
            for (const auto &run : text.runs) {
                QVERIFY(run.status == TextStatus::Unsupported);
                QVERIFY(!run.reason.empty());
            }
            QVERIFY_EXCEPTION_THROWN(doc->replaceText(0, {0, "A", {}}), Error);
            QVERIFY(!doc->canUndo());
        }
        auto scanned = openDocument(fixture("scanned.pdf"));
        QVERIFY(scanned->textRuns(0).runs.empty());
        QVERIFY(!scanned->textRuns(0).noTextReason.empty());
        auto marked = openDocument(fixture("marked.pdf"));
        marked->replaceText(0, {0, "Hi", {}});
        QCOMPARE(marked->textRuns(0).runs[0].text, std::string("Hi"));
        auto missing = openDocument(fixture("nonembedded-font.pdf"));
        QVERIFY(missing->textRuns(0).runs[0].status == TextStatus::NeedsFont);
        auto declarations = openDocument(fixture("conformance-declarations.pdf"));
        QCOMPARE(declarations->inspect().warnings.size(), std::size_t{2});
    }
    void protectedDocumentsFailExplicitly() {
        try {
            openDocument(fixture("encrypted.pdf"));
            QFAIL("Password-protected input unexpectedly opened");
        } catch (const Error &error) {
            QVERIFY(error.code() == ErrorCode::PasswordRequired);
        }
        auto doc = openDocument(fixture("signature-marker.pdf"));
        QVERIFY_EXCEPTION_THROWN(doc->replaceText(0, {0, "Hi", {}}), Error);
        QVERIFY(!doc->canUndo());
        const auto bytes = doc->pdfBytes();
        QFile original(QString::fromStdString(fixture("signature-marker.pdf")));
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto prefix = original.readAll();
        QVERIFY(bytes.size() >= std::size_t(prefix.size()));
        QVERIFY(
            std::equal(prefix.begin(), prefix.end(), reinterpret_cast<const char *>(bytes.data())));
        doc->allowSignedEdits(true);
        doc->replaceText(0, {0, "Hi", {}});
        const auto edited = doc->pdfBytes();
        QVERIFY(std::equal(prefix.begin(), prefix.end(),
                           reinterpret_cast<const char *>(edited.data())));
    }
    void previewsRecoveryAndPasswordSave() {
        auto doc = openDocument(fixture("subset-latin.pdf"));
        const auto original = doc->render(0).rgba;
        auto preview = doc->previewText(0, {0, "éè", {}});
        QVERIFY(preview.rgba != original);
        QCOMPARE(doc->render(0).rgba, original);
        QVERIFY(!doc->canUndo());
        QVERIFY(!doc->canRedo());
        doc->replaceText(0, {0, "éè", {}});
        const auto recovery = doc->recoveryData();
        QTemporaryDir dir;
        QFile file(dir.filePath("snapshot.pdf"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(reinterpret_cast<const char *>(recovery.pdf.data()), recovery.pdf.size());
        file.close();
        auto restored = openDocument(file.fileName().toStdString());
        restored->loadRecoveryJournal(recovery.journal);
        QCOMPARE(restored->textRuns(0).runs[0].text, std::string("éè"));
        QVERIFY(restored->canUndo());
        restored->undo();
        QCOMPARE(restored->render(0).rgba, original);
        restored->redo();
        auto protectedDoc = openDocument(fixture("encrypted.pdf"), "test-user");
        QVERIFY(protectedDoc->inspect().encrypted);
        protectedDoc->replaceText(0, {0, "Hi", {}});
        save(dir.filePath("encrypted.pdf"), *protectedDoc);
        QVERIFY_EXCEPTION_THROWN(openDocument(dir.filePath("encrypted.pdf").toStdString()), Error);
        auto decrypted = openDocument(dir.filePath("encrypted.pdf").toStdString(), "test-user");
        QCOMPARE(decrypted->textRuns(0).runs[0].text, std::string("Hi"));
        const auto protectedRecovery = protectedDoc->recoveryData();
        const std::string journal(protectedRecovery.journal.begin(),
                                  protectedRecovery.journal.end());
        QVERIFY(journal.starts_with("YoloEncryptedJournal1\n"));
        QFile encryptedSnapshot(dir.filePath("encrypted-snapshot.pdf"));
        QVERIFY(encryptedSnapshot.open(QIODevice::WriteOnly));
        encryptedSnapshot.write(reinterpret_cast<const char *>(protectedRecovery.pdf.data()),
                                protectedRecovery.pdf.size());
        encryptedSnapshot.close();
        auto recoveredProtected =
            openDocument(encryptedSnapshot.fileName().toStdString(), "test-user");
        recoveredProtected->loadRecoveryJournal(protectedRecovery.journal);
        recoveredProtected->undo();
        QCOMPARE(recoveredProtected->textRuns(0).runs[0].text, std::string("Hello yolopdf"));
    }
    void mixedLinePreservesStylesAndFollowingLineMatrix() {
        auto doc = openDocument(fixture("mixed-style.pdf"));
        auto lines = doc->textLines(0);
        QCOMPARE(lines.size(), std::size_t{2});
        QCOMPARE(lines[0].text, std::string("Hello World"));
        QCOMPARE(lines[0].segments.size(), std::size_t{2});
        const auto before = doc->textRuns(0);
        auto preview = doc->previewLine(0, {lines[0].id, "Hi World", {}});
        QVERIFY(preview.width > 0);
        QVERIFY(!doc->canUndo());
        QVERIFY(!doc->canRedo());
        doc->replaceLine(0, {lines[0].id, "Hi World", {}});
        auto after = doc->textRuns(0);
        QCOMPARE(after.runs[0].text, std::string("Hi "));
        QCOMPARE(after.runs[1].text, std::string("World"));
        QCOMPARE(after.runs[1].fontName, before.runs[1].fontName);
        QCOMPARE(after.runs[0].color, before.runs[0].color);
        QCOMPARE(after.runs[1].color, before.runs[1].color);
        QCOMPARE(after.runs[2].textMatrix, before.runs[2].textMatrix);
        doc->undo();
        QCOMPARE(doc->textLines(0)[0].text, lines[0].text);
        doc->redo();
        TextReplacement tint{0, "Hi World", {}};
        tint.changeColor = true;
        tint.color = {0, 1, 0};
        doc->replaceLine(0, tint);
        after = doc->textRuns(0);
        QCOMPARE(after.runs[0].color, std::vector<double>({0, 1, 0}));
        QCOMPARE(after.runs[2].color, before.runs[2].color);
        auto rotated = openDocument(fixture("nested.pdf"));
        rotated->replaceLine(0, {0, "Text", {}, 0, 10});
        QCOMPARE(rotated->textLines(0)[0].text, std::string("Text"));
    }
    void wrapOnlyIntoEmptySpaceAndKeepUndo() {
        auto doc = openDocument(fixture("basic.pdf"));
        TextReplacement edit{0, "Hello yolopdf Hello yolopdf", {}};
        edit.wrapWidth = 80;
        doc->replaceLine(0, edit);
        QVERIFY(doc->textLines(0).size() > 1);
        QTemporaryDir dir;
        save(dir.filePath("wrap.pdf"), *doc);
        auto reopened = openDocument(dir.filePath("wrap.pdf").toStdString());
        QVERIFY(!reopened->search("Hello").empty());
        doc->undo();
        QCOMPARE(doc->textLines(0)[0].text, std::string("Hello yolopdf"));
        doc->addAnnotation(0, {AnnotationType::Rectangle, {20, 72, 150, 95}, "obstruction"});
        const auto before = doc->render(0);
        QVERIFY_EXCEPTION_THROWN(doc->replaceLine(0, edit), Error);
        QCOMPARE(doc->render(0).rgba, before.rgba);
    }
    void pastedImageRetainsOriginalStreamAndAlpha() {
        auto source = openDocument(fixture("jpeg.pdf"));
        auto target = openDocument(fixture("basic.pdf"));
        const auto jpeg = source->originalJpeg(0, 0);
        source->copyImageTo(0, 0, *target, 0, {30, 0, 0, 20, 30, 30});
        source.reset();
        QCOMPARE(target->originalJpeg(0, 1), jpeg);
        target->undo();
        QCOMPARE(target->images(0).size(), std::size_t{1});
    }
    void textSizeSearchAndAnnotations() {
        auto doc = openDocument(fixture("basic.pdf"));
        QVERIFY(!doc->search("yolopdf").empty());
        QVERIFY(doc->copySelection(0, 20, 40, 170, 70).find("yolopdf") != std::string::npos);
        doc->replaceText(0, {0, "Hello yolopdf", {}, 0, 12});
        QCOMPARE(doc->textRuns(0).runs[0].fontSize, 12.0);
        doc->operation("Two annotations", [&](auto &d) {
            d.addAnnotation(0, {AnnotationType::Rectangle, {10, 10, 50, 30}, "box"});
            d.addAnnotation(0, {AnnotationType::Arrow, {60, 10, 90, 30}, "arrow"});
        });
        QCOMPARE(doc->annotations(0).size(), std::size_t{2});
        doc->undo();
        QVERIFY(doc->annotations(0).empty());
        doc->redo();
        auto a = doc->annotations(0)[0];
        doc->setAnnotationOpacity(0, a.id, .5);
        doc->deleteAnnotation(0, a.id);
        QCOMPARE(doc->annotations(0).size(), std::size_t{1});
        AddTextSpec text;
        text.fontFile = fixture("YoloTest-Regular.ttf");
        text.text = "Added Ω";
        text.x = 20;
        text.y = 100;
        doc->addText(0, text);
        QVERIFY(!doc->search("Added Ω").empty());
        QTemporaryDir dir;
        save(dir.filePath("added.pdf"), *doc);
        auto reopened = openDocument(dir.filePath("added.pdf").toStdString());
        QVERIFY(!reopened->search("Added Ω").empty());
    }
    void imageOccurrencesAndOriginalResolution() {
        auto doc = openDocument(fixture("shared-image.pdf"));
        const auto before = doc->render(0);
        QCOMPARE(doc->images(0).size(), std::size_t{3});
        const auto copy = doc->copyImage(0, 0);
        QCOMPARE(copy.width, 1);
        QCOMPARE(copy.height, 1);
        QCOMPARE(copy.rgba[0], std::uint8_t{255});
        doc->editImage(0, {0, ImageAction::Delete, {}, {}});
        QCOMPARE(doc->images(0).size(), std::size_t{2});
        auto after = doc->render(0);
        for (int y = 0; y < 200; ++y)
            for (int x = 80; x < 300; ++x)
                for (int c = 0; c < 4; ++c)
                    QCOMPARE(after.rgba[(y * 300 + x) * 4 + c], before.rgba[(y * 300 + x) * 4 + c]);
        doc->undo();
        QCOMPARE(doc->render(0).rgba, before.rgba);
        doc->editImage(0, {1, ImageAction::Replace, {}, fixture("blue.png")});
        QCOMPARE(doc->copyImage(0, 1).width, 2);
        QCOMPARE(doc->copyImage(0, 0).width, 1);
        auto matrix = doc->images(0)[0].transform;
        matrix[4] += 10;
        doc->editImage(0, {0, ImageAction::Transform, matrix, {}});
        QVERIFY(std::abs(doc->images(0)[0].transform[4] - matrix[4]) < .01);
    }
    void inlineAndNestedImages() {
        auto inlined = openDocument(fixture("inline-image.pdf"));
        QVERIFY(inlined->images(0)[0].inlineImage);
        inlined->editImage(0, {0, ImageAction::Replace, {}, fixture("blue.png")});
        QCOMPARE(inlined->copyImage(0, 0).width, 2);
        auto nested = openDocument(fixture("nested-image.pdf"));
        const auto other = nested->render(1);
        nested->editImage(0, {0, ImageAction::Replace, {}, fixture("blue.png")});
        QCOMPARE(nested->render(1).rgba, other.rgba);
        QCOMPARE(nested->copyImage(0, 0).width, 2);
        QCOMPARE(nested->copyImage(1, 0).width, 1);
    }
    void areaAndOriginalJpeg() {
        auto doc = openDocument(fixture("basic.pdf"));
        auto area = doc->copyArea(0, {20, 150, 60, 180}, 72);
        QCOMPARE(area.width, 40);
        QCOMPARE(area.height, 30);
        QVERIFY(std::abs(int(area.rgba[0]) - 51) <= 1);
        QVERIFY_EXCEPTION_THROWN(doc->copyArea(0, {-1, 0, 20, 30}, 72), Error);
        auto jpeg = openDocument(fixture("jpeg.pdf"));
        const auto bytes = jpeg->originalJpeg(0, 0);
        QFile source(QString::fromStdString(fixture("original.jpg")));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto expected = source.readAll();
        QVERIFY(bytes == std::vector<std::uint8_t>(expected.begin(), expected.end()));
    }
    void exactSizeStampAndFlatten() {
        auto doc = openDocument(fixture("nested.pdf"));
        StampSpec stamp;
        stamp.artworkFile = fixture("red-alpha.png");
        stamp.id = "test";
        stamp.xMm = 10;
        stamp.yMm = 10;
        doc->placeStamp(0, stamp);
        auto raster = doc->render(0);
        const int pixel = (40 * raster.width + 100) * 4;
        QCOMPARE(raster.rgba[pixel], std::uint8_t{255});
        QVERIFY(std::abs(int(raster.rgba[pixel + 1]) - 127) <= 2);
        // Flattening moves transparent paint into the page's isolated group.
        // MuPDF's integer compositing differs by one color level in this case.
        const auto stamped = raster.rgba;
        doc->flattenStamps();
        QVERIFY(withinOneLevel(doc->render(0).rgba, stamped));
        doc->undo();
        QCOMPARE(doc->render(0).rgba, stamped);
        QTemporaryDir dir;
        save(dir.filePath("stamp.pdf"), *doc);
        auto reopened = openDocument(dir.filePath("stamp.pdf").toStdString());
        QCOMPARE(reopened->render(0).rgba, stamped);
    }
    void stampMovementAndOpacityRetainCustomArtwork() {
        auto doc = createDocument();
        doc->insertBlankPage(0, 100, 100);
        StampSpec stamp;
        stamp.artworkFile = fixture("vector.svg");
        stamp.xMm = 10;
        stamp.yMm = 10;
        doc->placeStamp(0, stamp);
        auto original = doc->annotations(0).front();
        auto before = doc->render(0);
        auto bounds = original.bounds;
        bounds.x0 += 10;
        bounds.x1 += 10;
        bounds.y0 += 10;
        bounds.y1 += 10;
        doc->moveAnnotation(0, original.id, bounds);
        auto moved = doc->render(0);
        const int x = int((bounds.x0 + bounds.x1) / 2), y = int((bounds.y0 + bounds.y1) / 2),
                  pixel = (y * moved.width + x) * 4;
        QCOMPARE(moved.rgba[pixel], std::uint8_t{0});
        QCOMPARE(moved.rgba[pixel + 2], std::uint8_t{255});
        doc->setAnnotationOpacity(0, original.id, .5);
        auto translucent = doc->render(0);
        QVERIFY(translucent.rgba[pixel] > 100 && translucent.rgba[pixel] < 150);
        QVERIFY(translucent.rgba[pixel + 2] > translucent.rgba[pixel] + 30);
        doc->undo();
        QCOMPARE(doc->render(0).rgba, moved.rgba);
        doc->undo();
        QCOMPARE(doc->render(0).rgba, before.rgba);
    }
    void stampRotationDuplicateAndLockedSize() {
        auto doc = openDocument(fixture("basic.pdf"));
        StampSpec stamp;
        stamp.artworkFile = fixture("vector.svg");
        stamp.xMm = 10;
        stamp.yMm = 10;
        doc->placeStamp(0, stamp);
        const auto original = doc->annotations(0).front();
        const auto before = doc->render(0).rgba;
        auto resized = original.bounds;
        resized.x1 += 5;
        QVERIFY_EXCEPTION_THROWN(doc->moveAnnotation(0, original.id, resized), Error);
        QCOMPARE(doc->render(0).rgba, before);
        doc->duplicateStamp(0, original.id, 120, 0);
        QCOMPARE(doc->annotations(0).size(), std::size_t{2});
        auto duplicated = doc->render(0).rgba;
        doc->rotateStamp(0, original.id, 90);
        const auto rotated = doc->annotations(0).front();
        auto after = doc->render(0);
        for (int y = int(original.bounds.y0) + 1; y < int(original.bounds.y1) - 1; ++y)
            for (int x = int(original.bounds.x0) + 121; x < int(original.bounds.x1) + 119; ++x)
                for (int c = 0; c < 4; ++c)
                    QCOMPARE(after.rgba[(y * after.width + x) * 4 + c],
                             duplicated[(y * after.width + x) * 4 + c]);
        QVERIFY(std::abs((rotated.bounds.x1 - rotated.bounds.x0) -
                         (original.bounds.y1 - original.bounds.y0)) < .02);
        QVERIFY(std::abs((rotated.bounds.y1 - rotated.bounds.y0) -
                         (original.bounds.x1 - original.bounds.x0)) < .02);
        doc->undo();
        QCOMPARE(doc->render(0).rgba, duplicated);
        doc->undo();
        QCOMPARE(doc->render(0).rgba, before);
    }
    void vectorAndGifStamps() {
        auto doc = openDocument(fixture("basic.pdf"));
        StampSpec stamp;
        stamp.artworkFile = fixture("vector.svg");
        stamp.id = "vector";
        stamp.xMm = 10;
        stamp.yMm = 10;
        doc->placeStamp(0, stamp);
        QVERIFY(doc->render(0).rgba != openDocument(fixture("basic.pdf"))->render(0).rgba);
        auto gif = openDocument(fixture("basic.pdf"));
        stamp.artworkFile = fixture("frames.gif");
        stamp.gifFrame = 1;
        gif->placeStamp(0, stamp);
        const auto image = gif->render(0);
        const int pixel = (40 * 300 + 100) * 4;
        QCOMPARE(image.rgba[pixel + 2], std::uint8_t{255});
        QCOMPARE(image.rgba[pixel], std::uint8_t{0});
    }
    void pageTransactions() {
        auto doc = openDocument(fixture("basic.pdf"));
        doc->insertPages(1, {{fixture("nested.pdf"), {1, 0}}, {fixture("basic.pdf"), {}}});
        auto info = doc->inspect();
        QCOMPARE(info.pages.size(), std::size_t{4});
        QCOMPARE(info.pages[1].mediaBox.x1, 100.0);
        QCOMPARE(info.pages[2].userUnit, 2.0);
        doc->undo();
        QCOMPARE(doc->inspect().pages.size(), std::size_t{1});
        doc->redo();
        QCOMPARE(doc->inspect().pages.size(), std::size_t{4});
        doc->rearrangePages({3, 0, 0});
        doc->rotatePage(1, 90);
        info = doc->inspect();
        QCOMPARE(info.pages[1].rotation, 90);
        QCOMPARE(info.pages[2].rotation, 0);
        doc->insertBlankPage(0, 210, 297);
        info = doc->inspect();
        QVERIFY(std::abs(info.pages[0].mediaBox.x1 - 210 * 72 / 25.4) < .01);
        const auto before = doc->inspect().pages.size();
        QVERIFY_EXCEPTION_THROWN(
            doc->insertPages(0, {{fixture("basic.pdf"), {}}, {fixture("missing.pdf"), {}}}), Error);
        QCOMPARE(doc->inspect().pages.size(), before);
    }
};
QTEST_GUILESS_MAIN(Stage1Test)
#include "Stage1Test.moc"
