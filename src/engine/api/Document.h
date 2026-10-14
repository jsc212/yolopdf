// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace yolo::engine {

enum class ErrorCode {
    CannotOpen,
    InvalidDocument,
    PasswordRequired,
    WrongThread,
    InvalidPage,
    InvalidArgument,
    Unsupported,
    NeedsFont,
    Overflow,
    UnsafeEdit
};

class Error final : public std::runtime_error {
  public:
    Error(ErrorCode code, const std::string &message) : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }

  private:
    ErrorCode code_;
};

struct Rect {
    double x0{}, y0{}, x1{}, y1{};
};
struct FontInfo {
    std::string resourcePath;
    std::string name;
    std::string type;
    int objectNumber{};
    bool embedded{};
    bool subset{};
    bool hasToUnicode{};
};
struct ImageInfo {
    std::string resourcePath;
    int objectNumber{};
    int width{};
    int height{};
    bool hasAlpha{};
};
struct PageInfo {
    int index{};        // Zero-based in the engine.
    int id{};           // Object identity within this open document; stable across undo.
    Rect visibleBounds; // Display points after the engine's page transform.
    Rect mediaBox;
    Rect cropBox;
    int rotation{};
    double userUnit{1};
    std::vector<FontInfo> fonts;
    std::vector<ImageInfo> imageResources; // Resource inventory, not occurrence selection.
};
struct DocumentInfo {
    std::vector<PageInfo> pages;
    std::vector<std::string> warnings;
    bool encrypted{}, signedDocument{}, mayEdit{true}, mayCopy{true}, mayPrint{true};
};
struct RasterImage {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba;
};

using Matrix = std::array<double, 6>;
enum class TextStatus { OriginalFont, NeedsFont, Unsupported };
struct TextRun {
    int id{};
    std::string text;
    std::string fontName;
    double fontSize{};
    Rect bounds; // MuPDF display coordinates: points, top-left origin, page transform applied.
    Matrix textMatrix{};
    Matrix ctm{};
    double charSpacing{}, wordSpacing{}, horizontalScale{100}, rise{};
    int renderMode{};
    std::string colorSpace{"DeviceGray"};
    std::vector<double> color{0};
    TextStatus status{TextStatus::Unsupported};
    std::string reason;
};
struct TextPage {
    std::vector<TextRun> runs;
    std::string noTextReason;
};
struct TextReplacement {
    int runId{};
    std::string text;
    // An explicit imported font applies to the whole run; never selected silently.
    std::string fontFile;
    int fontIndex{};
    double fontSize{};    // Zero keeps original size.
    bool allowOverflow{}; // Explicit caller choice; the UI warns about overlap.
    bool changeColor{};
    std::array<double, 3> color{0, 0, 0};
    double wrapWidth{}; // Physical points; zero disables wrapping.
};
struct TextSegment {
    int runId{}, start{}, length{};
    TextRun style;
};
struct EditableLine {
    int id{};
    std::string text;
    Rect bounds;
    std::vector<TextSegment> segments;
    TextStatus status{TextStatus::OriginalFont};
    std::string reason;
};
struct HistoryState {
    int position{}, steps{};
    std::vector<std::string> labels;
};
struct RecoveryData {
    std::vector<std::uint8_t> pdf, journal;
};
struct SearchHit {
    int page{};
    Rect bounds;
};
struct AddTextSpec {
    std::string text, fontFile;
    int fontIndex{}, alignment{};
    double x{}, y{}, width{}, height{}, fontSize{12}; // Display points; y is baseline.
    std::array<double, 3> color{0, 0, 0};
};
enum class AnnotationType {
    Stamp,
    Highlight,
    Strikeout,
    Marker,
    Note,
    Arrow,
    Rectangle,
    Other,
    FreeText
};
using Point = std::array<double, 2>;
struct AnnotationInfo {
    int id{};
    AnnotationType type{};
    Rect bounds;
    std::string contents;
    double opacity{1};
};
struct AnnotationSpec {
    AnnotationType type{AnnotationType::Note};
    Rect bounds;
    std::string contents;
    std::array<double, 3> color{1, .8, 0};
    double opacity{1};
    std::vector<Point> points;    // One marker stroke, in displayed physical points.
    double thickness{8};          // Marker width in displayed physical points.
    std::vector<Point> selection; // Highlight/strikeout anchor and cursor, if supplied.
};
struct ImageOccurrence {
    int id{};
    Matrix transform{}; // Display coordinates; maps the PDF image unit square.
    Rect bounds;
    int width{}, height{};
    bool inlineImage{};
};
enum class ImageAction { Delete, Transform, Replace };
struct ImageEdit {
    int occurrenceId{};
    ImageAction action{ImageAction::Delete};
    Matrix transform{};
    std::string replacementFile;
};
struct StampSpec {
    std::string artworkFile;
    std::string id;
    double xMm{}, yMm{}, widthMm{40}, heightMm{20}; // Visible page top-left offset.
    double rotation{}, opacity{1};
    int gifFrame{};
};
struct PageSource {
    std::string path;
    std::vector<int> pages;
}; // Zero-based; empty means all.
class IRenderJob {
  public:
    virtual ~IRenderJob() = default;
    virtual RasterImage render(double dpi, const Rect &tile = {}) = 0;
};
class IRenderSource {
  public:
    virtual ~IRenderSource() = default;
    // Create jobs on the owning engine thread; each job may then run once on a worker.
    virtual std::unique_ptr<IRenderJob> job() = 0;
};

// Confined to its opening thread. No PDF-library types cross this boundary.
class IDocument {
  public:
    virtual ~IDocument() = default;
    virtual void operation(const std::string &label,
                           const std::function<void(IDocument &)> &action) = 0;
    virtual DocumentInfo inspect() = 0;
    virtual RasterImage render(int pageIndex, double dpi = 72) = 0;
    virtual std::shared_ptr<IRenderSource> renderSource(int pageIndex) = 0;
    virtual TextPage textRuns(int pageIndex) = 0;
    virtual std::vector<EditableLine> textLines(int pageIndex) = 0;
    virtual void replaceLine(int pageIndex, const TextReplacement &) = 0;
    virtual RasterImage previewLine(int pageIndex, const TextReplacement &, double dpi = 96) = 0;
    virtual std::vector<SearchHit> search(const std::string &query) = 0;
    virtual std::string copySelection(int pageIndex, double x0, double y0, double x1,
                                      double y1) = 0;
    virtual void replaceText(int pageIndex, const TextReplacement &replacement) = 0;
    virtual RasterImage previewText(int pageIndex, const TextReplacement &replacement,
                                    double dpi = 96) = 0;
    virtual void addText(int pageIndex, const AddTextSpec &spec) = 0;
    virtual std::vector<AnnotationInfo> annotations(int pageIndex) = 0;
    virtual void addAnnotation(int pageIndex, const AnnotationSpec &spec) = 0;
    virtual void moveAnnotation(int pageIndex, int id, const Rect &bounds) = 0;
    virtual void deleteAnnotation(int pageIndex, int id) = 0;
    virtual void setAnnotationOpacity(int pageIndex, int id, double opacity) = 0;
    virtual void rotateStamp(int pageIndex, int id, double degrees) = 0;
    virtual void duplicateStamp(int pageIndex, int id, double dx, double dy) = 0;
    virtual void allowSignedEdits(bool allow) = 0;
    virtual std::vector<ImageOccurrence> images(int pageIndex) = 0;
    virtual RasterImage copyImage(int pageIndex, int occurrenceId) = 0;
    virtual void copyImageTo(int pageIndex, int occurrenceId, IDocument &target, int targetPage,
                             const Matrix &) = 0;
    virtual std::vector<std::uint8_t> originalJpeg(int pageIndex, int occurrenceId) = 0;
    virtual RasterImage copyArea(int pageIndex, const Rect &bounds, double dpi = 300) = 0;
    virtual void editImage(int pageIndex, const ImageEdit &edit) = 0;
    virtual void insertImage(int pageIndex, const std::string &file,
                             const Matrix &displayTransform) = 0;
    virtual void placeStamp(int pageIndex, const StampSpec &stamp) = 0;
    virtual void flattenStamps() = 0;
    virtual std::vector<std::uint8_t> flattenedBytes() = 0; // Copy; leaves live history intact.
    virtual void insertPages(int at, const std::vector<PageSource> &sources) = 0;
    virtual void rearrangePages(const std::vector<int> &order) = 0;
    virtual void rotatePage(int pageIndex, int degrees) = 0;
    virtual void insertBlankPage(int at, double widthMm, double heightMm) = 0;
    virtual bool canUndo() = 0;
    virtual bool canRedo() = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual HistoryState history() = 0;
    virtual RecoveryData recoveryData() = 0;
    virtual void loadRecoveryJournal(const std::vector<std::uint8_t> &journal) = 0;
    virtual std::vector<std::uint8_t>
    pdfBytes() = 0; // Verified serialization; caller saves atomically.
};

std::unique_ptr<IDocument> openDocument(const std::string &utf8Path,
                                        const std::string &password = {});
std::unique_ptr<IDocument> createDocument();
std::string engineVersion();

} // namespace yolo::engine
