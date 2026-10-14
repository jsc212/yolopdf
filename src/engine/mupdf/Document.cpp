// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/api/Document.h"
#include "Content.h"
#include "Lines.h"
#include "Operations.h"
#include "Render.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QUuid>
#include <algorithm>
#include <cmath>
#include <set>
#include <thread>
#include <utility>

namespace yolo::engine {
namespace {

// Each callback contains only C operations and trivial locals: MuPDF errors use
// longjmp, so C++ objects requiring destructors must stay outside fz_try.
template <typename F> auto call(fz_context *ctx, F function) -> decltype(function()) {
    using T = decltype(function());
    T result{};
    fz_var(result);
    fz_try(ctx) {
        result = function();
    }
    fz_catch(ctx) {
        throw Error(ErrorCode::InvalidDocument, fz_caught_message(ctx));
    }
    return result;
}

pdf_obj *get(fz_context *ctx, pdf_obj *obj, const char *key) {
    return call(ctx, [&] { return pdf_dict_gets(ctx, obj, key); });
}
pdf_obj *inherited(fz_context *ctx, pdf_obj *obj, const char *key) {
    return call(ctx, [&] { return pdf_dict_gets_inheritable(ctx, obj, key); });
}
std::string name(fz_context *ctx, pdf_obj *obj) {
    return call(ctx, [&] { return pdf_to_name(ctx, obj); });
}
int number(fz_context *ctx, pdf_obj *obj) {
    return call(ctx, [&] { return pdf_to_int(ctx, obj); });
}
int objectNumber(fz_context *ctx, pdf_obj *obj) {
    return call(ctx, [&] { return pdf_to_num(ctx, obj); });
}
Rect box(fz_context *ctx, pdf_obj *obj) {
    const auto rect = call(ctx, [&] { return pdf_to_rect(ctx, obj); });
    return {rect.x0, rect.y0, rect.x1, rect.y1};
}

bool subsetName(const std::string &value) {
    return value.size() > 7 && value[6] == '+' &&
           std::all_of(value.begin(), value.begin() + 6,
                       [](char c) { return c >= 'A' && c <= 'Z'; });
}

class Document final : public IDocument {
  public:
    explicit Document(const std::string &path, bool create = false,
                      const std::string &password = {})
        : owner_(std::this_thread::get_id()), password_(password) {
        ctx_ = fz_new_context(nullptr, &locks_->callbacks, FZ_STORE_DEFAULT);
        if (!ctx_)
            throw Error(ErrorCode::CannotOpen, "Cannot allocate PDF engine context");
        try {
            doc_ = call(ctx_, [&] {
                return create ? pdf_create_document(ctx_) : pdf_open_document(ctx_, path.c_str());
            });
            if (call(ctx_, [&] { return pdf_needs_password(ctx_, doc_); }) && !call(ctx_, [&] {
                    return pdf_authenticate_password(ctx_, doc_, password.c_str());
                }))
                throw Error(ErrorCode::PasswordRequired, "This PDF requires a password");
            mupdf::checked(ctx_, [&] { pdf_enable_journal(ctx_, doc_); });
        } catch (...) {
            pdf_drop_document(ctx_, doc_);
            fz_drop_context(ctx_);
            throw;
        }
    }

    ~Document() override {
        pdf_drop_document(ctx_, doc_);
        fz_drop_context(ctx_);
    }

    DocumentInfo inspect() override {
        checkThread();
        DocumentInfo info;
        info.encrypted = doc_->crypt != nullptr;
        info.signedDocument = mupdf::isSigned(ctx_, doc_);
        info.mayEdit =
            call(ctx_, [&] { return pdf_has_permission(ctx_, doc_, FZ_PERMISSION_EDIT); });
        info.mayCopy =
            call(ctx_, [&] { return pdf_has_permission(ctx_, doc_, FZ_PERMISSION_COPY); });
        info.mayPrint =
            call(ctx_, [&] { return pdf_has_permission(ctx_, doc_, FZ_PERMISSION_PRINT); });
        auto *catalog =
            call(ctx_, [&] { return pdf_dict_get(ctx_, pdf_trailer(ctx_, doc_), PDF_NAME(Root)); });
        if (get(ctx_, catalog, "StructTreeRoot"))
            info.warnings.push_back(
                "Tagged PDF: edits are not a guarantee of structure or accessibility conformance");
        if (auto *metadata = get(ctx_, catalog, "Metadata")) {
            try {
                mupdf::Ref<fz_stream, fz_drop_stream> stream(
                    ctx_, call(ctx_, [&] { return pdf_open_stream(ctx_, metadata); }));
                std::array<unsigned char, 65536> data{};
                const auto size = call(
                    ctx_, [&] { return fz_read(ctx_, stream.get(), data.data(), data.size()); });
                const std::string xmp(reinterpret_cast<const char *>(data.data()), size);
                if (xmp.find("http://www.aiim.org/pdfa/ns/id/") != std::string::npos ||
                    xmp.find("pdfaid:") != std::string::npos)
                    info.warnings.push_back(
                        "PDF/A declaration detected: editing may invalidate PDF/A conformance");
                if (size == data.size())
                    info.warnings.push_back(
                        "XMP exceeds the PDF/A detection limit; conformance status is unknown");
            } catch (const Error &) {
                info.warnings.push_back("XMP could not be inspected; PDF/A status is unknown");
            }
        }
        const int count = call(ctx_, [&] { return pdf_count_pages(ctx_, doc_); });
        for (int i = 0; i < count; ++i) {
            PageInfo page;
            page.index = i;
            pdf_obj *obj = call(ctx_, [&] { return pdf_lookup_page_obj(ctx_, doc_, i); });
            page.id = objectNumber(ctx_, obj);
            auto loaded = loadedPage(i);
            page.visibleBounds =
                mupdf::rect(call(ctx_, [&] { return fz_bound_page(ctx_, loaded.get()); }));
            page.mediaBox = box(ctx_, inherited(ctx_, obj, "MediaBox"));
            auto *crop = inherited(ctx_, obj, "CropBox");
            page.cropBox = crop ? box(ctx_, crop) : page.mediaBox;
            page.rotation = ((number(ctx_, inherited(ctx_, obj, "Rotate")) % 360) + 360) % 360;
            auto *unit =
                get(ctx_, obj, "UserUnit"); // UserUnit is not an inheritable page attribute.
            if (unit)
                page.userUnit = call(ctx_, [&] { return pdf_to_real(ctx_, unit); });
            std::set<pdf_obj *> ancestors;
            scanResources(inherited(ctx_, obj, "Resources"), "", page, ancestors, 0);
            info.pages.push_back(std::move(page));
        }
        return info;
    }
    void operation(const std::string &label,
                   const std::function<void(IDocument &)> &action) override {
        checkThread();
        mupdf::Transaction transaction(ctx_, doc_, label.c_str(), signedAllowed_);
        action(*this);
        transaction.commit();
    }

    std::shared_ptr<IRenderSource> renderSource(int pageIndex) override {
        checkThread();
        return mupdf::makeRenderSource(ctx_, doc_, pageIndex, locks_);
    }
    RasterImage render(int pageIndex, double dpi = 72) override {
        checkThread();
        const int count = call(ctx_, [&] { return pdf_count_pages(ctx_, doc_); });
        if (pageIndex < 0 || pageIndex >= count)
            throw Error(ErrorCode::InvalidPage, "Page index is out of range");
        if (!std::isfinite(dpi) || dpi < 1 || dpi > 1200)
            throw Error(ErrorCode::InvalidPage, "DPI must be between 1 and 1200");
        // Bound allocation even for hostile page dimensions or extreme UserUnit.
        auto *page = call(ctx_, [&] { return pdf_load_page(ctx_, doc_, pageIndex); });
        fz_pixmap *pixmap = nullptr;
        try {
            auto bounds = call(ctx_, [&] { return pdf_bound_page(ctx_, page, FZ_CROP_BOX); });
            const double width = std::ceil((bounds.x1 - bounds.x0) * dpi / 72);
            const double height = std::ceil((bounds.y1 - bounds.y0) * dpi / 72);
            if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 ||
                width * height > 64 * 1024 * 1024)
                throw Error(ErrorCode::InvalidPage, "Rendered page exceeds the 64 megapixel limit");
            pixmap = call(ctx_, [&] {
                return fz_new_pixmap_from_page(
                    ctx_, reinterpret_cast<fz_page *>(page),
                    fz_scale(static_cast<float>(dpi / 72), static_cast<float>(dpi / 72)),
                    fz_device_rgb(ctx_), 0);
            });
            RasterImage image;
            image.width = fz_pixmap_width(ctx_, pixmap);
            image.height = fz_pixmap_height(ctx_, pixmap);
            image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
            const auto *samples = fz_pixmap_samples(ctx_, pixmap);
            const int stride = fz_pixmap_stride(ctx_, pixmap);
            for (int y = 0; y < image.height; ++y) {
                for (int x = 0; x < image.width; ++x) {
                    auto *pixel = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
                    const auto *source = samples + y * stride + x * 3;
                    std::copy_n(source, 3, pixel);
                    pixel[3] = 255;
                }
            }
            fz_drop_pixmap(ctx_, pixmap);
            fz_drop_page(ctx_, reinterpret_cast<fz_page *>(page));
            return image;
        } catch (...) {
            fz_drop_pixmap(ctx_, pixmap);
            fz_drop_page(ctx_, reinterpret_cast<fz_page *>(page));
            throw;
        }
    }

    TextPage textRuns(int pageIndex) override {
        checkThread();
        const auto trace = mupdf::tracePage(ctx_, doc_, pageIndex);
        TextPage result;
        for (const auto &run : trace->runs)
            result.runs.push_back(run.info);
        if (result.runs.empty())
            result.noTextReason = "No content-stream text: scanned, outlined, or empty page";
        return result;
    }
    std::vector<EditableLine> textLines(int page) override {
        checkThread();
        return mupdf::buildLines(*mupdf::tracePage(ctx_, doc_, page));
    }
    void replaceLine(int page, const TextReplacement &edit) override {
        checkThread();
        auto trace = mupdf::tracePage(ctx_, doc_, page);
        auto lines = mupdf::buildLines(*trace);
        auto line =
            std::find_if(lines.begin(), lines.end(), [&](auto &l) { return l.id == edit.runId; });
        if (line == lines.end())
            throw Error(ErrorCode::InvalidArgument, "Text line no longer exists");
        const auto before = render(page, 144);
        mupdf::Transaction transaction(ctx_, doc_, "Edit text line", signedAllowed_);
        if (edit.fontSize != 0 &&
            (!std::isfinite(edit.fontSize) || edit.fontSize < 1 || edit.fontSize > 1200))
            throw Error(ErrorCode::InvalidArgument, "Choose a font size from 1–1200 points");
        const auto expected = edit.wrapWidth > 0
                                  ? mupdf::writeWrappedLine(ctx_, doc_, page, *trace, *line, edit)
                                  : mupdf::writeLine(ctx_, doc_, page, *trace, *line, edit);
        auto afterTrace = mupdf::tracePage(ctx_, doc_, page);
        auto allowed = mupdf::rect(line->bounds);
        for (auto &part : expected) {
            if (part.runId >= int(afterTrace->runs.size()))
                throw Error(ErrorCode::UnsafeEdit, "Text segment disappeared");
            auto &run = afterTrace->runs[part.runId];
            if (!part.text.empty() && !run.clipSafe)
                throw Error(ErrorCode::UnsafeEdit,
                            "Edited text would extend beyond its clipping region");
            auto m = fz_concat(run.start, mupdf::matrix(run.info.ctm));
            if (run.info.text != part.text ||
                (!part.font.empty() && run.info.fontName != part.font) ||
                std::hypot(m.e - part.baseline.x, m.f - part.baseline.y) > .05)
                throw Error(ErrorCode::UnsafeEdit,
                            "Text segment, font or baseline verification failed");
            if (!part.text.empty())
                allowed = fz_union_rect(allowed, mupdf::rect(run.info.bounds));
            if (edit.wrapWidth > 0) {
                auto box = fz_expand_rect(mupdf::rect(run.info.bounds), 1);
                if (box.x0 < 0 || box.y0 < 0 || box.x1 * 2 > before.width ||
                    box.y1 * 2 > before.height)
                    throw Error(ErrorCode::Overflow, "Wrapped glyphs exceed the visible page");
                const auto old = mupdf::rect(line->bounds);
                for (int y = std::max(0, int(std::floor(box.y0 * 2)));
                     y < std::min(before.height, int(std::ceil(box.y1 * 2))); ++y)
                    for (int x = std::max(0, int(std::floor(box.x0 * 2)));
                         x < std::min(before.width, int(std::ceil(box.x1 * 2))); ++x) {
                        if (x >= std::floor(old.x0 * 2) && x < std::ceil(old.x1 * 2) &&
                            y >= std::floor(old.y0 * 2) && y < std::ceil(old.y1 * 2))
                            continue;
                        const auto at = (std::size_t(y) * before.width + x) * 4;
                        if (before.rgba[at] != 255 || before.rgba[at + 1] != 255 ||
                            before.rgba[at + 2] != 255)
                            throw Error(
                                ErrorCode::Overflow,
                                "Wrap is only allowed into empty white space in this version");
                    }
            }
        }
        auto normalize = [](const std::string &s) {
            std::string out;
            bool space = false;
            for (unsigned char c : s) {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                    space = true;
                    continue;
                }
                if (space && !out.empty())
                    out += ' ';
                space = false;
                out += char(c);
            }
            return out;
        };
        if (!edit.text.empty() &&
            normalize(mupdf::extractText(ctx_, doc_, page)).find(normalize(edit.text)) ==
                std::string::npos)
            throw Error(ErrorCode::UnsafeEdit, "Edited line did not extract as expected");
        const auto after = render(page, 144);
        allowed = fz_expand_rect(allowed, 1);
        if (before.width != after.width || before.height != after.height)
            throw Error(ErrorCode::UnsafeEdit, "Page geometry changed");
        for (int y = 0; y < before.height; ++y)
            for (int x = 0; x < before.width; ++x) {
                if (x >= std::floor(allowed.x0 * 2) && x < std::ceil(allowed.x1 * 2) &&
                    y >= std::floor(allowed.y0 * 2) && y < std::ceil(allowed.y1 * 2))
                    continue;
                auto offset = (std::size_t(y) * before.width + x) * 4;
                if (!std::equal(before.rgba.begin() + offset, before.rgba.begin() + offset + 4,
                                after.rgba.begin() + offset))
                    throw Error(ErrorCode::UnsafeEdit,
                                "Render changed outside the edited line; edit was rolled back");
            }
        transaction.commit();
    }
    RasterImage previewLine(int page, const TextReplacement &edit, double dpi) override {
        checkThread();
        mupdf::Transaction preview(ctx_, doc_, "Line preview", signedAllowed_);
        replaceLine(page, edit);
        return render(page, dpi);
    }
    void allowSignedEdits(bool allow) override {
        checkThread();
        signedAllowed_ = allow;
    }
    std::vector<SearchHit> search(const std::string &query) override {
        checkThread();
        std::vector<SearchHit> result;
        if (query.empty())
            return result;
        const int count = call(ctx_, [&] { return pdf_count_pages(ctx_, doc_); });
        for (int i = 0; i < count; ++i) {
            auto text = structured(i);
            std::array<fz_quad, 2048> quads{};
            std::array<int, 2048> marks{};
            const int n = call(ctx_, [&] {
                return fz_search_stext_page(ctx_, text.get(), query.c_str(), marks.data(),
                                            quads.data(), quads.size());
            });
            for (int j = 0; j < n; ++j)
                result.push_back({i, mupdf::rect(fz_rect_from_quad(quads[j]))});
        }
        return result;
    }
    std::string copySelection(int page, double x0, double y0, double x1, double y1) override {
        checkThread();
        if (!call(ctx_, [&] { return pdf_has_permission(ctx_, doc_, FZ_PERMISSION_COPY); }))
            throw Error(ErrorCode::Unsupported, "PDF permission restrictions disallow copying");
        auto text = structured(page);
        char *copy = call(ctx_, [&] {
            return fz_copy_selection(ctx_, text.get(), {float(x0), float(y0)},
                                     {float(x1), float(y1)}, 0);
        });
        std::string result(copy ? copy : "");
        fz_free(ctx_, copy);
        return result;
    }
    void addText(int page, const AddTextSpec &spec) override {
        checkThread();
        mupdf::Transaction operation(ctx_, doc_, "Add text", signedAllowed_);
        mupdf::appendText(ctx_, doc_, page, spec);
        operation.commit();
    }
    std::vector<AnnotationInfo> annotations(int index) override {
        checkThread();
        auto page = loadedPage(index);
        std::vector<AnnotationInfo> result;
        for (auto *a = call(
                 ctx_,
                 [&] { return pdf_first_annot(ctx_, reinterpret_cast<pdf_page *>(page.get())); });
             a; a = call(ctx_, [&] { return pdf_next_annot(ctx_, a); })) {
            const auto type = call(ctx_, [&] { return pdf_annot_type(ctx_, a); });
            AnnotationType own = type == PDF_ANNOT_STAMP        ? AnnotationType::Stamp
                                 : type == PDF_ANNOT_HIGHLIGHT  ? AnnotationType::Highlight
                                 : type == PDF_ANNOT_STRIKE_OUT ? AnnotationType::Strikeout
                                 : type == PDF_ANNOT_INK        ? AnnotationType::Marker
                                 : type == PDF_ANNOT_TEXT       ? AnnotationType::Note
                                 : type == PDF_ANNOT_LINE       ? AnnotationType::Arrow
                                 : type == PDF_ANNOT_SQUARE     ? AnnotationType::Rectangle
                                 : type == PDF_ANNOT_FREE_TEXT  ? AnnotationType::FreeText
                                                                : AnnotationType::Other;
            auto *obj = call(ctx_, [&] { return pdf_annot_obj(ctx_, a); });
            result.push_back({objectNumber(ctx_, obj), own,
                              mupdf::rect(call(ctx_, [&] { return pdf_bound_annot(ctx_, a); })),
                              call(ctx_, [&] { return pdf_annot_contents(ctx_, a); }),
                              call(ctx_, [&] { return pdf_annot_opacity(ctx_, a); })});
        }
        return result;
    }
    void addAnnotation(int index, const AnnotationSpec &spec) override {
        checkThread();
        if (spec.type != AnnotationType::Marker)
            validateRect(spec.bounds);
        const auto type = spec.type == AnnotationType::Note        ? PDF_ANNOT_TEXT
                          : spec.type == AnnotationType::Highlight ? PDF_ANNOT_HIGHLIGHT
                          : spec.type == AnnotationType::Strikeout ? PDF_ANNOT_STRIKE_OUT
                          : spec.type == AnnotationType::Marker    ? PDF_ANNOT_INK
                          : spec.type == AnnotationType::Arrow     ? PDF_ANNOT_LINE
                          : spec.type == AnnotationType::Rectangle ? PDF_ANNOT_SQUARE
                                                                   : PDF_ANNOT_UNKNOWN;
        if (type == PDF_ANNOT_UNKNOWN)
            throw Error(ErrorCode::InvalidArgument, "Unsupported annotation type");
        for (double value : spec.color)
            if (!std::isfinite(value) || value < 0 || value > 1)
                throw Error(ErrorCode::InvalidArgument, "Invalid colour");
        if (!std::isfinite(spec.opacity) || spec.opacity < 0 || spec.opacity > 1)
            throw Error(ErrorCode::InvalidArgument, "Invalid opacity");
        auto page = loadedPage(index);
        std::vector<fz_quad> highlights;
        if (type == PDF_ANNOT_HIGHLIGHT || type == PDF_ANNOT_STRIKE_OUT) {
            auto text = structured(index);
            highlights.resize(2048);
            const auto r = mupdf::rect(spec.bounds);
            fz_point start{r.x0, r.y0}, end{r.x1, r.y1};
            if (!spec.selection.empty()) {
                if (spec.selection.size() != 2)
                    throw Error(ErrorCode::InvalidArgument,
                                "Text markings need two selection points");
                for (const auto &point : spec.selection)
                    if (!std::isfinite(point[0]) || !std::isfinite(point[1]))
                        throw Error(ErrorCode::InvalidArgument, "Invalid text selection point");
                start = {float(spec.selection[0][0]), float(spec.selection[0][1])};
                end = {float(spec.selection[1][0]), float(spec.selection[1][1])};
            }
            const int n = call(ctx_, [&] {
                return fz_highlight_selection(ctx_, text.get(), start, end, highlights.data(),
                                              highlights.size());
            });
            if (n == 0 || n >= int(highlights.size()))
                throw Error(ErrorCode::Unsupported,
                            "Select text to mark; selection must be fewer than 2048 lines");
            highlights.resize(n);
        }
        std::vector<fz_point> stroke;
        double strokeWidth = spec.thickness;
        if (type == PDF_ANNOT_INK) {
            if (spec.points.size() < 2 || spec.points.size() > 4096 ||
                !std::isfinite(strokeWidth) || strokeWidth < .25 || strokeWidth > 144)
                throw Error(ErrorCode::InvalidArgument,
                            "Marker strokes require 2–4096 points and a width of .25–144 pt");
            for (const auto &p : spec.points) {
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || std::abs(p[0]) > 1e7 ||
                    std::abs(p[1]) > 1e7)
                    throw Error(ErrorCode::InvalidArgument, "Invalid marker coordinates");
                stroke.push_back({float(p[0]), float(p[1])});
            }
            const auto matrix = mupdf::pageTransform(ctx_, doc_, index);
            const auto scale = std::hypot(matrix.a, matrix.b);
            if (scale < .0001)
                throw Error(ErrorCode::InvalidDocument, "Invalid page scale");
            strokeWidth /= scale;
        }
        mupdf::Transaction operation(ctx_, doc_, "Add annotation", signedAllowed_);
        auto *a = call(ctx_, [&] {
            return pdf_create_annot(ctx_, reinterpret_cast<pdf_page *>(page.get()), type);
        });
        const auto r = mupdf::rect(spec.bounds);
        const float color[] = {float(spec.color[0]), float(spec.color[1]), float(spec.color[2])};
        mupdf::checked(ctx_, [&] {
            if (type == PDF_ANNOT_LINE) {
                pdf_set_annot_line(ctx_, a, {r.x0, r.y0}, {r.x1, r.y1});
                pdf_set_annot_line_end_style(ctx_, a, PDF_ANNOT_LE_CLOSED_ARROW);
            } else if (type == PDF_ANNOT_HIGHLIGHT || type == PDF_ANNOT_STRIKE_OUT) {
                pdf_set_annot_quad_points(ctx_, a, highlights.size(), highlights.data());
            } else if (type == PDF_ANNOT_INK) {
                const int count = stroke.size();
                pdf_set_annot_ink_list(ctx_, a, 1, &count, stroke.data());
                pdf_set_annot_border_width(ctx_, a, strokeWidth);
            } else
                pdf_set_annot_rect(ctx_, a, r);
            pdf_set_annot_color(ctx_, a, 3, color);
            pdf_set_annot_opacity(ctx_, a, spec.opacity);
            pdf_set_annot_contents(ctx_, a, spec.contents.c_str());
            pdf_set_annot_flags(ctx_, a, PDF_ANNOT_IS_PRINT);
            pdf_update_annot(ctx_, a);
        });
        operation.commit();
    }
    void moveAnnotation(int index, int id, const Rect &bounds) override {
        checkThread();
        validateRect(bounds);
        auto page = loadedPage(index);
        auto *a = findAnnotation(page.get(), id);
        auto type = call(ctx_, [&] { return pdf_annot_type(ctx_, a); });
        if (type != PDF_ANNOT_STAMP && type != PDF_ANNOT_TEXT && type != PDF_ANNOT_SQUARE)
            throw Error(ErrorCode::Unsupported,
                        "Move is supported for stamps, notes and rectangles");
        if (type == PDF_ANNOT_STAMP) {
            auto *obj = call(ctx_, [&] { return pdf_annot_obj(ctx_, a); });
            const bool locked = call(ctx_, [&] {
                return pdf_to_bool(
                    ctx_, pdf_dict_gets(ctx_, pdf_dict_gets(ctx_, obj, "YoloStamp"), "SizeLocked"));
            });
            const auto old = call(ctx_, [&] { return pdf_bound_annot(ctx_, a); });
            if (locked && (std::abs((bounds.x1 - bounds.x0) - (old.x1 - old.x0)) > .01 ||
                           std::abs((bounds.y1 - bounds.y0) - (old.y1 - old.y0)) > .01))
                throw Error(ErrorCode::Unsupported, "This stamp's physical size is locked");
        }
        mupdf::Transaction operation(ctx_, doc_, "Move annotation", signedAllowed_);
        mupdf::checked(ctx_, [&] {
            pdf_set_annot_rect(ctx_, a, mupdf::rect(bounds));
            if (type == PDF_ANNOT_STAMP)
                pdf_set_annot_resynthesised(ctx_, a);
            else
                pdf_update_annot(ctx_, a);
        });
        operation.commit();
    }
    void deleteAnnotation(int index, int id) override {
        checkThread();
        auto page = loadedPage(index);
        auto *a = findAnnotation(page.get(), id);
        mupdf::Transaction operation(ctx_, doc_, "Delete annotation", signedAllowed_);
        mupdf::checked(
            ctx_, [&] { pdf_delete_annot(ctx_, reinterpret_cast<pdf_page *>(page.get()), a); });
        operation.commit();
    }
    void setAnnotationOpacity(int index, int id, double opacity) override {
        checkThread();
        if (!std::isfinite(opacity) || opacity < 0 || opacity > 1)
            throw Error(ErrorCode::InvalidArgument, "Invalid opacity");
        auto page = loadedPage(index);
        auto *a = findAnnotation(page.get(), id);
        mupdf::Transaction operation(ctx_, doc_, "Annotation opacity", signedAllowed_);
        const auto type = call(ctx_, [&] { return pdf_annot_type(ctx_, a); });
        if (type == PDF_ANNOT_STAMP) {
            auto *appearance = call(ctx_, [&] { return pdf_annot_ap(ctx_, a); });
            if (!appearance)
                throw Error(ErrorCode::Unsupported, "Stamp has no appearance");
            auto *source =
                call(ctx_, [&] { return pdf_dict_get(ctx_, appearance, PDF_NAME(Resources)); });
            mupdf::Object resources(ctx_, call(ctx_, [&] {
                                        return pdf_deep_copy_obj(
                                            ctx_, pdf_resolve_indirect(ctx_, source));
                                    }));
            auto *gs = call(ctx_, [&] {
                return pdf_dict_gets(ctx_, pdf_dict_get(ctx_, resources.get(), PDF_NAME(ExtGState)),
                                     "YoloOpacity");
            });
            if (!gs)
                throw Error(ErrorCode::Unsupported,
                            "Opacity editing is supported for yolopdf stamps");
            mupdf::Buffer data(ctx_, call(ctx_, [&] { return pdf_load_stream(ctx_, appearance); }));
            mupdf::checked(ctx_, [&] {
                pdf_set_obj_parent(ctx_, resources.get(), 0);
                pdf_dict_put_real(ctx_, gs, PDF_NAME(ca), opacity);
                pdf_dict_put_real(ctx_, gs, PDF_NAME(CA), opacity);
                pdf_set_annot_opacity(ctx_, a, opacity);
                const auto matrix = pdf_xobject_matrix(ctx_, appearance);
                const auto bbox = pdf_xobject_bbox(ctx_, appearance);
                pdf_dict_del(ctx_, pdf_dict_get(ctx_, pdf_annot_obj(ctx_, a), PDF_NAME(AP)),
                             PDF_NAME(N));
                pdf_set_annot_appearance(ctx_, a, "N", nullptr, matrix, bbox, resources.get(),
                                         data.get());
            });
        } else
            mupdf::checked(ctx_, [&] {
                pdf_set_annot_opacity(ctx_, a, opacity);
                pdf_update_annot(ctx_, a);
            });
        operation.commit();
    }
    void rotateStamp(int index, int id, double degrees) override {
        checkThread();
        if (!std::isfinite(degrees))
            throw Error(ErrorCode::InvalidArgument, "Invalid stamp rotation");
        auto page = loadedPage(index);
        auto *a = findAnnotation(page.get(), id);
        if (call(ctx_, [&] { return pdf_annot_type(ctx_, a); }) != PDF_ANNOT_STAMP)
            throw Error(ErrorCode::Unsupported, "Select a stamp to rotate");
        auto *obj = call(ctx_, [&] { return pdf_annot_obj(ctx_, a); });
        auto *appearance = call(ctx_, [&] { return pdf_annot_ap(ctx_, a); });
        if (!appearance)
            throw Error(ErrorCode::Unsupported, "Stamp has no appearance");
        const auto bounds = call(ctx_, [&] { return pdf_bound_annot(ctx_, a); });
        const float cx = (bounds.x0 + bounds.x1) / 2, cy = (bounds.y0 + bounds.y1) / 2;
        auto delta =
            fz_concat(fz_concat(fz_translate(-cx, -cy), fz_rotate(degrees)), fz_translate(cx, cy));
        auto transform = mupdf::pageTransform(ctx_, doc_, index);
        auto matrix = call(ctx_, [&] {
            return fz_concat(pdf_xobject_matrix(ctx_, appearance), pdf_annot_transform(ctx_, a));
        });
        matrix =
            fz_concat(fz_concat(fz_concat(matrix, transform), delta), fz_invert_matrix(transform));
        auto bbox = call(ctx_, [&] { return pdf_xobject_bbox(ctx_, appearance); });
        mupdf::Transaction operation(ctx_, doc_, "Rotate stamp", signedAllowed_);
        mupdf::Buffer data(ctx_, call(ctx_, [&] { return pdf_load_stream(ctx_, appearance); }));
        mupdf::checked(ctx_, [&] {
            auto *resources = pdf_dict_get(ctx_, appearance, PDF_NAME(Resources));
            pdf_dict_put_rect(ctx_, obj, PDF_NAME(Rect), fz_transform_rect(bbox, matrix));
            pdf_dict_del(ctx_, pdf_dict_get(ctx_, obj, PDF_NAME(AP)), PDF_NAME(N));
            pdf_set_annot_appearance(ctx_, a, "N", nullptr, matrix, bbox, resources, data.get());
            auto *metadata = pdf_dict_gets(ctx_, obj, "YoloStamp");
            if (metadata)
                pdf_dict_puts_drop(
                    ctx_, metadata, "Rot",
                    pdf_new_real(ctx_, pdf_to_real(ctx_, pdf_dict_gets(ctx_, metadata, "Rot")) +
                                           degrees));
        });
        operation.commit();
    }
    void duplicateStamp(int index, int id, double dx, double dy) override {
        checkThread();
        if (!std::isfinite(dx) || !std::isfinite(dy))
            throw Error(ErrorCode::InvalidArgument, "Invalid stamp offset");
        auto page = loadedPage(index);
        auto *source = findAnnotation(page.get(), id);
        if (call(ctx_, [&] { return pdf_annot_type(ctx_, source); }) != PDF_ANNOT_STAMP)
            throw Error(ErrorCode::Unsupported, "Select a stamp to duplicate");
        auto *obj = call(ctx_, [&] { return pdf_annot_obj(ctx_, source); });
        auto r = call(ctx_, [&] { return pdf_bound_annot(ctx_, source); });
        r.x0 += dx;
        r.x1 += dx;
        r.y0 += dy;
        r.y1 += dy;
        mupdf::Object copy(ctx_, call(ctx_, [&] {
                               return pdf_deep_copy_obj(ctx_, pdf_resolve_indirect(ctx_, obj));
                           }));
        mupdf::checked(ctx_, [&] { pdf_set_obj_parent(ctx_, copy.get(), 0); });
        mupdf::Transaction operation(ctx_, doc_, "Duplicate stamp", signedAllowed_);
        mupdf::checked(ctx_, [&] {
            auto *annot =
                pdf_create_annot(ctx_, reinterpret_cast<pdf_page *>(page.get()), PDF_ANNOT_STAMP);
            auto *target = pdf_annot_obj(ctx_, annot);
            for (int i = 0; i < pdf_dict_len(ctx_, copy.get()); ++i) {
                auto *key = pdf_dict_get_key(ctx_, copy.get(), i);
                if (pdf_name_eq(ctx_, key, PDF_NAME(P)))
                    continue;
                pdf_dict_put(ctx_, target, key, pdf_dict_get_val(ctx_, copy.get(), i));
            }
            auto *metadata = pdf_dict_gets(ctx_, target, "YoloStamp");
            if (metadata)
                pdf_dict_puts_drop(
                    ctx_, metadata, "Id",
                    pdf_new_text_string(
                        ctx_,
                        QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8().constData()));
            pdf_set_annot_rect(ctx_, annot, r);
            pdf_set_annot_resynthesised(ctx_, annot);
        });
        operation.commit();
    }
    void replaceText(int pageIndex, const TextReplacement &replacement) override {
        checkThread();
        auto beforeTrace = mupdf::tracePage(ctx_, doc_, pageIndex);
        if (replacement.runId < 0 || replacement.runId >= int(beforeTrace->runs.size()))
            throw Error(ErrorCode::InvalidArgument, "Unknown text run");
        const auto &original = beforeTrace->runs[replacement.runId];
        const auto before = render(pageIndex, 144);
        mupdf::Transaction operation(ctx_, doc_, "Replace text", signedAllowed_);
        mupdf::writeTextReplacement(ctx_, doc_, pageIndex, original, replacement);
        const auto afterTrace = mupdf::tracePage(ctx_, doc_, pageIndex);
        if (replacement.runId >= int(afterTrace->runs.size()))
            throw Error(ErrorCode::UnsafeEdit, "Edited run disappeared");
        const auto &changed = afterTrace->runs[replacement.runId];
        if (!replacement.text.empty() && !changed.clipSafe)
            throw Error(ErrorCode::UnsafeEdit,
                        "Edited text would extend beyond its clipping region");
        if (changed.info.text != replacement.text ||
            (replacement.fontFile.empty() && changed.info.fontName != original.info.fontName) ||
            std::abs(changed.start.e - original.start.e) > .01 ||
            std::abs(changed.start.f - original.start.f) > .01)
            throw Error(ErrorCode::UnsafeEdit,
                        "Replacement text, font or baseline verification failed");
        if (!replacement.text.empty() &&
            mupdf::extractText(ctx_, doc_, pageIndex).find(replacement.text) == std::string::npos)
            throw Error(ErrorCode::UnsafeEdit, "Structured-text verification failed");
        const auto after = render(pageIndex, 144);
        const auto allowed = fz_expand_rect(
            fz_union_rect(mupdf::rect(original.info.bounds), mupdf::rect(changed.info.bounds)), 1);
        if (before.width != after.width || before.height != after.height)
            throw Error(ErrorCode::UnsafeEdit, "Replacement changed page geometry");
        for (int y = 0; y < before.height; ++y)
            for (int x = 0; x < before.width; ++x) {
                if (x >= std::floor(allowed.x0 * 2) && x < std::ceil(allowed.x1 * 2) &&
                    y >= std::floor(allowed.y0 * 2) && y < std::ceil(allowed.y1 * 2))
                    continue;
                const auto offset = (std::size_t(y) * before.width + x) * 4;
                if (!std::equal(before.rgba.begin() + offset, before.rgba.begin() + offset + 4,
                                after.rgba.begin() + offset))
                    throw Error(ErrorCode::UnsafeEdit,
                                "Render changed outside the edited bounds; operation rolled back");
            }
        operation.commit();
    }
    RasterImage previewText(int pageIndex, const TextReplacement &replacement,
                            double dpi) override {
        checkThread();
        mupdf::Transaction preview(ctx_, doc_, "Text preview", signedAllowed_);
        replaceText(pageIndex, replacement);
        return render(pageIndex, dpi);
    }
    std::vector<ImageOccurrence> images(int pageIndex) override {
        checkThread();
        const auto trace = mupdf::tracePage(ctx_, doc_, pageIndex);
        std::vector<ImageOccurrence> result;
        for (const auto &image : trace->images)
            result.push_back(image.info);
        return result;
    }
    RasterImage copyImage(int pageIndex, int occurrenceId) override {
        checkThread();
        if (!call(ctx_, [&] { return pdf_has_permission(ctx_, doc_, FZ_PERMISSION_COPY); }))
            throw Error(ErrorCode::Unsupported, "PDF permission restrictions disallow copying");
        auto trace = mupdf::tracePage(ctx_, doc_, pageIndex);
        if (occurrenceId < 0 || occurrenceId >= int(trace->images.size()))
            throw Error(ErrorCode::InvalidArgument, "Unknown image occurrence");
        return mupdf::decodeImage(ctx_, trace->images[occurrenceId].image);
    }
    void copyImageTo(int pageIndex, int occurrenceId, IDocument &other, int targetPage,
                     const Matrix &transform) override {
        checkThread();
        auto *target = dynamic_cast<Document *>(&other);
        if (!target)
            throw Error(ErrorCode::Unsupported, "Image graft requires compatible engine documents");
        target->checkThread();
        if (!call(ctx_, [&] { return pdf_has_permission(ctx_, doc_, FZ_PERMISSION_COPY); }))
            throw Error(ErrorCode::Unsupported, "PDF permission restrictions disallow copying");
        auto trace = mupdf::tracePage(ctx_, doc_, pageIndex);
        if (occurrenceId < 0 || occurrenceId >= int(trace->images.size()))
            throw Error(ErrorCode::InvalidArgument, "Unknown image occurrence");
        auto &source = trace->images[occurrenceId];
        mupdf::Transaction operation(target->ctx_, target->doc_, "Paste image",
                                     target->signedAllowed_);
        mupdf::Ref<pdf_graft_map, pdf_drop_graft_map> map(
            target->ctx_,
            call(target->ctx_, [&] { return pdf_new_graft_map(target->ctx_, target->doc_); }));
        mupdf::Object copied(
            target->ctx_,
            source.object ? call(target->ctx_,
                                 [&] {
                                     return target == this
                                                ? pdf_keep_obj(target->ctx_, source.object)
                                                : pdf_graft_mapped_object(target->ctx_, map.get(),
                                                                          source.object);
                                 })
                          : call(target->ctx_, [&] {
                                return pdf_add_image(target->ctx_, target->doc_, source.image);
                            }));
        mupdf::appendImageObject(target->ctx_, target->doc_, targetPage, copied.get(), transform);
        operation.commit();
    }
    std::vector<std::uint8_t> originalJpeg(int pageIndex, int occurrenceId) override {
        checkThread();
        if (!call(ctx_, [&] { return pdf_has_permission(ctx_, doc_, FZ_PERMISSION_COPY); }))
            throw Error(ErrorCode::Unsupported, "PDF permission restrictions disallow copying");
        auto trace = mupdf::tracePage(ctx_, doc_, pageIndex);
        if (occurrenceId < 0 || occurrenceId >= int(trace->images.size()))
            throw Error(ErrorCode::InvalidArgument, "Unknown image occurrence");
        auto *image = trace->images[occurrenceId].image;
        if (image->mask || image->use_colorkey)
            throw Error(ErrorCode::Unsupported,
                        "Masked images must be exported as PNG to retain alpha");
        auto *compressed =
            mupdf::checked(ctx_, [&] { return fz_compressed_image_buffer(ctx_, image); });
        if (!compressed || compressed->params.type != FZ_IMAGE_JPEG)
            throw Error(ErrorCode::Unsupported, "This occurrence is not an original JPEG stream");
        unsigned char *bytes;
        const auto size = mupdf::checked(
            ctx_, [&] { return fz_buffer_storage(ctx_, compressed->buffer, &bytes); });
        return {bytes, bytes + size};
    }
    RasterImage copyArea(int pageIndex, const Rect &bounds, double dpi = 300) override {
        checkThread();
        if (!call(ctx_, [&] { return pdf_has_permission(ctx_, doc_, FZ_PERMISSION_COPY); }))
            throw Error(ErrorCode::Unsupported, "PDF permission restrictions disallow copying");
        mupdf::pageObject(ctx_, doc_, pageIndex);
        for (double value : {bounds.x0, bounds.y0, bounds.x1, bounds.y1, dpi})
            if (!std::isfinite(value))
                throw Error(ErrorCode::InvalidArgument, "Area bounds and DPI must be finite");
        if (dpi < 1 || dpi > 1200 || bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0)
            throw Error(ErrorCode::InvalidArgument, "Invalid area or DPI");
        mupdf::Page page(ctx_, mupdf::checked(ctx_, [&] {
                             return reinterpret_cast<fz_page *>(
                                 pdf_load_page(ctx_, doc_, pageIndex));
                         }));
        const auto pageBounds =
            mupdf::checked(ctx_, [&] { return fz_bound_page(ctx_, page.get()); });
        if (bounds.x0 < pageBounds.x0 || bounds.y0 < pageBounds.y0 || bounds.x1 > pageBounds.x1 ||
            bounds.y1 > pageBounds.y1)
            throw Error(ErrorCode::InvalidArgument, "Area lies outside the visible page");
        const auto matrix = fz_scale(dpi / 72, dpi / 72);
        const auto scaled = fz_transform_rect(mupdf::rect(bounds), matrix);
        for (float value : {scaled.x0, scaled.y0, scaled.x1, scaled.y1})
            if (!std::isfinite(value) || std::abs(value) > 10000000)
                throw Error(ErrorCode::InvalidArgument, "Area coordinates exceed renderer limits");
        const auto clip = fz_round_rect(scaled);
        const auto width = std::int64_t(clip.x1) - clip.x0,
                   height = std::int64_t(clip.y1) - clip.y0;
        if (width < 1 || height < 1 || width * height > 64 * 1024 * 1024)
            throw Error(ErrorCode::InvalidArgument, "Area exceeds renderer limits");
        mupdf::Ref<fz_pixmap, fz_drop_pixmap> pixmap(
            ctx_, mupdf::checked(ctx_, [&] {
                return fz_new_pixmap_with_bbox(ctx_, fz_device_rgb(ctx_), clip, nullptr, 0);
            }));
        mupdf::Ref<fz_device, fz_drop_device> device(
            ctx_, mupdf::checked(
                      ctx_, [&] { return fz_new_draw_device(ctx_, fz_identity, pixmap.get()); }));
        mupdf::checked(ctx_, [&] {
            fz_clear_pixmap_with_value(ctx_, pixmap.get(), 255);
            fz_run_page(ctx_, page.get(), device.get(), matrix, nullptr);
            fz_close_device(ctx_, device.get());
        });
        mupdf::Image image(ctx_, mupdf::checked(ctx_, [&] {
                               return fz_new_image_from_pixmap(ctx_, pixmap.get(), nullptr);
                           }));
        return mupdf::decodeImage(ctx_, image.get());
    }
    void editImage(int pageIndex, const ImageEdit &edit) override {
        checkThread();
        auto trace = mupdf::tracePage(ctx_, doc_, pageIndex);
        if (edit.occurrenceId < 0 || edit.occurrenceId >= int(trace->images.size()))
            throw Error(ErrorCode::InvalidArgument, "Unknown image occurrence");
        mupdf::Transaction operation(ctx_, doc_, "Edit image occurrence", signedAllowed_);
        mupdf::writeImageEdit(ctx_, doc_, pageIndex, trace->images[edit.occurrenceId], edit);
        operation.commit();
    }
    void insertImage(int pageIndex, const std::string &file, const Matrix &transform) override {
        checkThread();
        mupdf::Transaction operation(ctx_, doc_, "Insert image", signedAllowed_);
        mupdf::appendImage(ctx_, doc_, pageIndex, file, transform);
        operation.commit();
    }
    void placeStamp(int pageIndex, const StampSpec &stamp) override {
        checkThread();
        mupdf::Transaction operation(ctx_, doc_, "Place stamp", signedAllowed_);
        mupdf::addStamp(ctx_, doc_, pageIndex, stamp);
        operation.commit();
    }
    void flattenStamps() override {
        checkThread();
        mupdf::Transaction operation(ctx_, doc_, "Flatten stamps", signedAllowed_);
        mupdf::flattenStamps(ctx_, doc_);
        operation.commit();
    }
    void insertPages(int at, const std::vector<PageSource> &sources) override {
        checkThread();
        mupdf::Transaction operation(ctx_, doc_, "Insert pages", signedAllowed_);
        mupdf::insertPages(ctx_, doc_, at, sources);
        operation.commit();
    }
    void rearrangePages(const std::vector<int> &order) override {
        checkThread();
        const int count = call(ctx_, [&] { return pdf_count_pages(ctx_, doc_); });
        if (order.empty())
            throw Error(ErrorCode::InvalidArgument, "A PDF must retain at least one page");
        for (int page : order)
            if (page < 0 || page >= count)
                throw Error(ErrorCode::InvalidPage, "Invalid page in new order");
        mupdf::Transaction operation(ctx_, doc_, "Rearrange pages", signedAllowed_);
        mupdf::rearrange(ctx_, doc_, order);
        operation.commit();
    }
    void rotatePage(int pageIndex, int degrees) override {
        checkThread();
        if (degrees % 90)
            throw Error(ErrorCode::InvalidArgument,
                        "Page rotation must be a multiple of 90 degrees");
        auto *obj = mupdf::pageObject(ctx_, doc_, pageIndex);
        auto *old = inherited(ctx_, obj, "Rotate");
        const int current = number(ctx_, old);
        const int rotation = int((std::int64_t(current) + degrees) % 360 + 360) % 360;
        mupdf::Transaction operation(ctx_, doc_, "Rotate page", signedAllowed_);
        mupdf::checked(ctx_, [&] { pdf_dict_put_int(ctx_, obj, PDF_NAME(Rotate), rotation); });
        operation.commit();
    }
    void insertBlankPage(int at, double width, double height) override {
        checkThread();
        mupdf::Transaction operation(ctx_, doc_, "Insert blank page", signedAllowed_);
        mupdf::blankPage(ctx_, doc_, at, width, height);
        operation.commit();
    }
    bool canUndo() override {
        checkThread();
        return call(ctx_, [&] { return pdf_can_undo(ctx_, doc_); });
    }
    bool canRedo() override {
        checkThread();
        return call(ctx_, [&] { return pdf_can_redo(ctx_, doc_); });
    }
    void undo() override {
        checkThread();
        mupdf::checked(ctx_, [&] {
            pdf_undo(ctx_, doc_);
            mupdf::invalidateCaches(ctx_, doc_);
        });
    }
    void redo() override {
        checkThread();
        mupdf::checked(ctx_, [&] {
            pdf_redo(ctx_, doc_);
            mupdf::invalidateCaches(ctx_, doc_);
        });
    }
    std::vector<std::uint8_t> pdfBytes() override {
        checkThread();
        auto result = mupdf::serialize(ctx_, doc_);
        mupdf::Ref<fz_stream, fz_drop_stream> input(
            ctx_, call(ctx_, [&] { return fz_open_memory(ctx_, result.data(), result.size()); }));
        mupdf::Ref<pdf_document, pdf_drop_document> verified(
            ctx_, call(ctx_, [&] { return pdf_open_document_with_stream(ctx_, input.get()); }));
        if (call(ctx_, [&] { return pdf_needs_password(ctx_, verified.get()); }) &&
            !call(ctx_, [&] {
                return pdf_authenticate_password(ctx_, verified.get(), password_.c_str());
            }))
            throw Error(ErrorCode::UnsafeEdit, "Saved encryption could not be verified");
        if ((verified.get()->crypt != nullptr) != (doc_->crypt != nullptr) ||
            call(ctx_, [&] { return pdf_count_pages(ctx_, verified.get()); }) !=
                call(ctx_, [&] { return pdf_count_pages(ctx_, doc_); }))
            throw Error(ErrorCode::UnsafeEdit,
                        "Saved PDF encryption or page count changed unexpectedly");
        return result;
    }
    std::vector<std::uint8_t> flattenedBytes() override {
        checkThread();
        mupdf::ensureEditable(ctx_, doc_, signedAllowed_);
        const auto bytes = pdfBytes();
        mupdf::Ref<fz_stream, fz_drop_stream> stream(
            ctx_, call(ctx_, [&] { return fz_open_memory(ctx_, bytes.data(), bytes.size()); }));
        mupdf::Ref<pdf_document, pdf_drop_document> copy(
            ctx_, call(ctx_, [&] { return pdf_open_document_with_stream(ctx_, stream.get()); }));
        if (call(ctx_, [&] { return pdf_needs_password(ctx_, copy.get()); }) && !call(ctx_, [&] {
                return pdf_authenticate_password(ctx_, copy.get(), password_.c_str());
            }))
            throw Error(ErrorCode::UnsafeEdit, "Cannot unlock flattened copy");
        mupdf::checked(ctx_, [&] { pdf_bake_document(ctx_, copy.get(), 1, 0); });
        auto result = mupdf::serialize(ctx_, copy.get());
        mupdf::Ref<fz_stream, fz_drop_stream> verifyStream(
            ctx_, call(ctx_, [&] { return fz_open_memory(ctx_, result.data(), result.size()); }));
        mupdf::Ref<pdf_document, pdf_drop_document> verified(
            ctx_,
            call(ctx_, [&] { return pdf_open_document_with_stream(ctx_, verifyStream.get()); }));
        if (call(ctx_, [&] { return pdf_needs_password(ctx_, verified.get()); }) &&
            !call(ctx_, [&] {
                return pdf_authenticate_password(ctx_, verified.get(), password_.c_str());
            }))
            throw Error(ErrorCode::UnsafeEdit, "Flattened encryption could not be verified");
        if ((verified.get()->crypt != nullptr) != (doc_->crypt != nullptr) ||
            call(ctx_, [&] { return pdf_count_pages(ctx_, verified.get()); }) !=
                call(ctx_, [&] { return pdf_count_pages(ctx_, doc_); }))
            throw Error(ErrorCode::UnsafeEdit, "Flattened copy changed encryption or page count");
        return result;
    }
    HistoryState history() override {
        checkThread();
        HistoryState result;
        result.position = call(ctx_, [&] { return pdf_undoredo_state(ctx_, doc_, &result.steps); });
        for (int i = 0; i < result.steps; ++i)
            result.labels.push_back(call(ctx_, [&] { return pdf_undoredo_step(ctx_, doc_, i); }));
        return result;
    }
    RecoveryData recoveryData() override {
        checkThread();
        RecoveryData result;
        for (int part = 0; part < 2; ++part) {
            mupdf::Buffer bytes(ctx_, call(ctx_, [&] { return fz_new_buffer(ctx_, 4096); }));
            mupdf::Ref<fz_output, fz_drop_output> out(
                ctx_, call(ctx_, [&] { return fz_new_output_with_buffer(ctx_, bytes.get()); }));
            mupdf::checked(ctx_, [&] {
                if (part == 0)
                    mupdf::writeSnapshot(ctx_, doc_, out.get());
                else
                    pdf_write_journal(ctx_, doc_, out.get());
                fz_close_output(ctx_, out.get());
            });
            unsigned char *data;
            auto size = call(ctx_, [&] { return fz_buffer_storage(ctx_, bytes.get(), &data); });
            (part == 0 ? result.pdf : result.journal).assign(data, data + size);
        }
        if (doc_->crypt) {
            if (std::string(pdf_crypt_string_method(ctx_, doc_->crypt)) == "None")
                throw Error(ErrorCode::Unsupported,
                            "This PDF encryption cannot protect recovery history");
            mupdf::Buffer cipher(
                ctx_, call(ctx_, [&] { return fz_new_buffer(ctx_, result.journal.size() + 64); }));
            mupdf::checked(ctx_, [&] {
                pdf_encrypt_data(
                    ctx_, doc_->crypt, 0, 0,
                    [](fz_context *c, void *out, const unsigned char *data, size_t n) {
                        fz_append_data(c, static_cast<fz_buffer *>(out), data, n);
                    },
                    cipher.get(), result.journal.data(), result.journal.size());
            });
            unsigned char *data;
            auto n = call(ctx_, [&] { return fz_buffer_storage(ctx_, cipher.get(), &data); });
            const std::string prefix = "YoloEncryptedJournal1\n";
            result.journal.assign(prefix.begin(), prefix.end());
            result.journal.insert(result.journal.end(), data, data + n);
        }
        return result;
    }
    void loadRecoveryJournal(const std::vector<std::uint8_t> &journal) override {
        checkThread();
        if (history().steps != 0)
            throw Error(ErrorCode::InvalidArgument,
                        "Load a recovery journal only into a fresh document");
        const std::string marker = "YoloEncryptedJournal1\n";
        std::vector<std::uint8_t> plain = journal;
        if (journal.size() >= marker.size() &&
            std::equal(marker.begin(), marker.end(), journal.begin())) {
            if (!doc_->crypt)
                throw Error(ErrorCode::UnsafeEdit,
                            "Encrypted recovery history requires its encrypted PDF");
            mupdf::Object cipher(ctx_, call(ctx_, [&] {
                                     return pdf_new_string(ctx_,
                                                           reinterpret_cast<const char *>(
                                                               journal.data() + marker.size()),
                                                           journal.size() - marker.size());
                                 }));
            mupdf::checked(ctx_, [&] { pdf_crypt_obj(ctx_, doc_->crypt, cipher.get(), 0, 0); });
            const auto n = call(ctx_, [&] { return pdf_to_str_len(ctx_, cipher.get()); });
            auto *data = call(ctx_, [&] { return pdf_to_str_buf(ctx_, cipher.get()); });
            plain.assign(data, data + n);
        } else if (doc_->crypt)
            throw Error(ErrorCode::UnsafeEdit, "Encrypted PDF recovery history must be encrypted");
        mupdf::Ref<fz_stream, fz_drop_stream> in(
            ctx_, call(ctx_, [&] { return fz_open_memory(ctx_, plain.data(), plain.size()); }));
        pdf_drop_journal(ctx_, doc_->journal);
        doc_->journal = nullptr;
        try {
            mupdf::readJournal(ctx_, doc_, in.get());
        } catch (...) {
            mupdf::checked(ctx_, [&] { pdf_enable_journal(ctx_, doc_); });
            throw;
        }
        mupdf::checked(ctx_, [&] { mupdf::invalidateCaches(ctx_, doc_); });
        if (!doc_->journal) {
            mupdf::checked(ctx_, [&] { pdf_enable_journal(ctx_, doc_); });
            throw Error(ErrorCode::UnsafeEdit, "Recovery journal does not match its PDF snapshot");
        }
    }

  private:
    mupdf::Page loadedPage(int index) {
        mupdf::pageObject(ctx_, doc_, index);
        return mupdf::Page(ctx_, call(ctx_, [&] {
                               return reinterpret_cast<fz_page *>(pdf_load_page(ctx_, doc_, index));
                           }));
    }
    mupdf::Ref<fz_stext_page, fz_drop_stext_page> structured(int index) {
        auto page = loadedPage(index);
        return {ctx_,
                call(ctx_, [&] { return fz_new_stext_page_from_page(ctx_, page.get(), nullptr); })};
    }
    void validateRect(const Rect &r) {
        for (double v : {r.x0, r.y0, r.x1, r.y1})
            if (!std::isfinite(v) || std::abs(v) > 1000000)
                throw Error(ErrorCode::InvalidArgument, "Invalid rectangle");
        if (r.x1 <= r.x0 || r.y1 <= r.y0)
            throw Error(ErrorCode::InvalidArgument, "Rectangle must have positive dimensions");
    }
    pdf_annot *findAnnotation(fz_page *page, int id) {
        for (auto *a = call(
                 ctx_, [&] { return pdf_first_annot(ctx_, reinterpret_cast<pdf_page *>(page)); });
             a; a = call(ctx_, [&] { return pdf_next_annot(ctx_, a); })) {
            if (call(ctx_, [&] { return pdf_to_num(ctx_, pdf_annot_obj(ctx_, a)); }) == id)
                return a;
        }
        throw Error(ErrorCode::InvalidArgument, "Annotation is no longer present");
    }
    void checkThread() const {
        if (owner_ != std::this_thread::get_id())
            throw Error(ErrorCode::WrongThread, "A document must be used on its opening thread");
    }

    void scanResources(pdf_obj *resources, const std::string &path, PageInfo &page,
                       std::set<pdf_obj *> &ancestors, int depth) {
        if (!resources)
            return;
        auto *resolved = call(ctx_, [&] { return pdf_resolve_indirect(ctx_, resources); });
        if (depth > 64)
            throw Error(ErrorCode::InvalidDocument, "Form resource nesting exceeds 64 levels");
        if (!ancestors.insert(resolved).second)
            return;
        auto *fonts = get(ctx_, resources, "Font");
        const int fontCount = call(ctx_, [&] { return pdf_dict_len(ctx_, fonts); });
        for (int i = 0; i < fontCount; ++i) {
            auto *key = call(ctx_, [&] { return pdf_dict_get_key(ctx_, fonts, i); });
            auto *font = call(ctx_, [&] { return pdf_dict_get_val(ctx_, fonts, i); });
            FontInfo info;
            info.resourcePath = path + "/" + name(ctx_, key);
            info.objectNumber = objectNumber(ctx_, font);
            info.name = name(ctx_, get(ctx_, font, "BaseFont"));
            info.type = name(ctx_, get(ctx_, font, "Subtype"));
            info.subset = subsetName(info.name);
            info.hasToUnicode = get(ctx_, font, "ToUnicode") != nullptr;
            auto *descriptorFont = font;
            if (info.type == "Type0") {
                auto *descendants = get(ctx_, font, "DescendantFonts");
                descriptorFont = call(ctx_, [&] { return pdf_array_get(ctx_, descendants, 0); });
            }
            auto *descriptor = get(ctx_, descriptorFont, "FontDescriptor");
            info.embedded = get(ctx_, descriptor, "FontFile") ||
                            get(ctx_, descriptor, "FontFile2") ||
                            get(ctx_, descriptor, "FontFile3") ||
                            (info.type == "Type3" && get(ctx_, font, "CharProcs"));
            page.fonts.push_back(std::move(info));
        }
        auto *objects = get(ctx_, resources, "XObject");
        const int objectCount = call(ctx_, [&] { return pdf_dict_len(ctx_, objects); });
        for (int i = 0; i < objectCount; ++i) {
            auto *key = call(ctx_, [&] { return pdf_dict_get_key(ctx_, objects, i); });
            auto *obj = call(ctx_, [&] { return pdf_dict_get_val(ctx_, objects, i); });
            const auto objectPath = path + "/" + name(ctx_, key);
            const auto type = name(ctx_, get(ctx_, obj, "Subtype"));
            if (type == "Image") {
                page.imageResources.push_back(
                    {objectPath, objectNumber(ctx_, obj), number(ctx_, get(ctx_, obj, "Width")),
                     number(ctx_, get(ctx_, obj, "Height")),
                     get(ctx_, obj, "SMask") != nullptr || get(ctx_, obj, "Mask") != nullptr});
            } else if (type == "Form") {
                scanResources(get(ctx_, obj, "Resources"), objectPath, page, ancestors, depth + 1);
            }
        }
        ancestors.erase(resolved);
    }

    fz_context *ctx_{};
    std::shared_ptr<mupdf::ContextLocks> locks_{std::make_shared<mupdf::ContextLocks>()};
    pdf_document *doc_{};
    std::thread::id owner_;
    bool signedAllowed_{};
    std::string password_;
};
} // namespace

std::unique_ptr<IDocument> openDocument(const std::string &utf8Path, const std::string &password) {
    return std::make_unique<Document>(utf8Path, false, password);
}
std::unique_ptr<IDocument> createDocument() {
    return std::make_unique<Document>("", true);
}
std::string engineVersion() {
    return FZ_VERSION;
}
} // namespace yolo::engine
