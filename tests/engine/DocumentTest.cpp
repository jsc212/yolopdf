// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/api/Document.h"
#include <QTest>
#include <future>

using namespace yolo::engine;
namespace {
std::string fixture(const char *name) {
    return std::string(YOLOPDF_CORPUS_DIR) + "/" + name;
}
} // namespace
class DocumentTest : public QObject {
    Q_OBJECT
  private slots:
    void inspectBasic() {
        auto doc = openDocument(fixture("basic.pdf"));
        const auto info = doc->inspect();
        QCOMPARE(info.pages.size(), std::size_t{1});
        const auto &page = info.pages.front();
        QCOMPARE(page.mediaBox.x1, 300.0);
        QCOMPARE(page.cropBox.y1, 200.0);
        QCOMPARE(page.fonts.size(), std::size_t{1});
        QCOMPARE(page.fonts.front().name, std::string("Helvetica"));
        QVERIFY(!page.fonts.front().embedded);
        QVERIFY(!page.fonts.front().subset);
        QCOMPARE(page.imageResources.size(), std::size_t{1});
        QCOMPARE(page.imageResources.front().width, 1);
    }
    void inspectEmbedded() {
        auto doc = openDocument(fixture("embedded.pdf"));
        const auto font = doc->inspect().pages.front().fonts.front();
        QVERIFY(font.embedded);
        QVERIFY(font.subset);
        QCOMPARE(font.name, std::string("ABCDEF+NotoSans"));
    }
    void inheritedAndNestedResources() {
        auto doc = openDocument(fixture("nested.pdf"));
        const auto info = doc->inspect();
        QCOMPARE(info.pages.size(), std::size_t{2});
        QCOMPARE(info.pages[0].rotation, 90);
        QCOMPARE(info.pages[0].userUnit, 2.0);
        QCOMPARE(info.pages[0].fonts.front().resourcePath, std::string("/Shared/F1"));
        QCOMPARE(info.pages[1].rotation, 270);
        QCOMPARE(info.pages[1].cropBox.x0, 10.0);
        QCOMPARE(info.pages[1].userUnit, 1.0);
        const auto first = doc->render(0);
        QCOMPARE(first.width, 400);
        QCOMPARE(first.height, 600);
        const auto second = doc->render(1);
        QCOMPARE(second.width, 110);
        QCOMPARE(second.height, 80);
    }
    void cyclicResourcesTerminate() {
        auto doc = openDocument(fixture("cyclic-resources.pdf"));
        QCOMPARE(doc->inspect().pages.size(), std::size_t{2});
    }
    void actualRendering() {
        auto doc = openDocument(fixture("basic.pdf"));
        const auto raster = doc->render(0);
        QCOMPARE(raster.width, 300);
        QCOMPARE(raster.height, 200);
        const auto pixel = [&](int x, int y, int component) {
            return raster.rgba[(y * raster.width + x) * 4 + component];
        };
        QCOMPARE(pixel(1, 1, 0), std::uint8_t{255});
        QVERIFY(std::abs(int(pixel(30, 160, 0)) - 51) <= 1);
        QVERIFY(std::abs(int(pixel(30, 160, 1)) - 102) <= 1);
        QVERIFY(std::abs(int(pixel(30, 160, 2)) - 153) <= 1);
        QCOMPARE(pixel(110, 140, 0), std::uint8_t{255});
        QCOMPARE(pixel(110, 140, 1), std::uint8_t{0});
    }
    void rejectInvalidInput() {
        QVERIFY_EXCEPTION_THROWN(openDocument(fixture("invalid.pdf")), Error);
        QVERIFY_EXCEPTION_THROWN(openDocument(fixture("missing.pdf")), Error);
        auto doc = openDocument(fixture("basic.pdf"));
        QVERIFY_EXCEPTION_THROWN(doc->render(-1), Error);
        QVERIFY_EXCEPTION_THROWN(doc->render(1), Error);
        QVERIFY_EXCEPTION_THROWN(doc->render(0, 0), Error);
        QVERIFY_EXCEPTION_THROWN(doc->render(0, 1201), Error);
    }
    void rejectCrossThreadUse() {
        auto doc = openDocument(fixture("basic.pdf"));
        const auto result = std::async(std::launch::async, [&] {
                                try {
                                    doc->inspect();
                                } catch (const Error &error) {
                                    return error.code() == ErrorCode::WrongThread;
                                }
                                return false;
                            }).get();
        QVERIFY(result);
        QCOMPARE(doc->inspect().pages.size(), std::size_t{1});
    }
};
QTEST_GUILESS_MAIN(DocumentTest)
#include "DocumentTest.moc"
