// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Support.h"
#include <map>
#include <memory>
#include <vector>

namespace yolo::engine::mupdf {
struct FormStep {
    int occurrence{};
    pdf_obj *form{};
}; // Borrowed; document stays open.
struct Glyph {
    int unicode{};
    fz_point origin{};
};
struct TracedRun {
    TextRun info;
    std::vector<FormStep> forms;
    int showIndex{};
    pdf_font_desc *font{};
    pdf_obj *fontObject{};
    std::string fontResource;
    pdf_text_state textState{};
    fz_matrix start{}, end{};
    fz_matrix lineMatrix{};
    double advance{};
    std::vector<Glyph> glyphs;
};
struct TracedImage {
    ImageOccurrence info;
    std::vector<FormStep> forms;
    int occurrence{};
    fz_image *image{};
    pdf_obj *object{};
    fz_matrix ctm{};
};
struct Trace {
    explicit Trace(fz_context *context) : ctx(context) {}
    ~Trace();
    Trace(const Trace &) = delete;
    fz_context *ctx;
    std::vector<TracedRun> runs;
    std::vector<TracedImage> images;
    std::vector<pdf_font_desc *> fonts;
};
std::unique_ptr<Trace> tracePage(fz_context *, pdf_document *, int);
double writeTextReplacement(fz_context *, pdf_document *, int, const TracedRun &,
                            const TextReplacement &, const fz_matrix *start = nullptr);
std::vector<int> editCharacters(const std::string &);
std::string editString(const std::vector<int> &);
struct EncodedText {
    explicit EncodedText(fz_context *ctx) : font(ctx) {}
    Object font;
    std::string bytes;
    double advance{}, size{};
    bool replacesFont{};
};
EncodedText encodeReplacement(fz_context *, pdf_document *, const TracedRun &,
                              const TextReplacement &);
void replaceShowWithForm(fz_context *, pdf_document *, int, const TracedRun &, pdf_obj *);
void writeImageEdit(fz_context *, pdf_document *, int, const TracedImage &, const ImageEdit &);
void appendImage(fz_context *, pdf_document *, int, const std::string &, const Matrix &);
void appendImageObject(fz_context *, pdf_document *, int, pdf_obj *, const Matrix &);
void appendText(fz_context *, pdf_document *, int, const AddTextSpec &);
RasterImage decodeImage(fz_context *, fz_image *);
std::string extractText(fz_context *, pdf_document *, int);
pdf_obj *copyPageResources(fz_context *, pdf_document *, int);
void appendContent(fz_context *, pdf_document *, int, pdf_obj *, fz_buffer *);
std::string rewriteAppearance(fz_context *, pdf_document *, pdf_obj *, pdf_obj *,
                              const std::map<std::string, std::string> &);
} // namespace yolo::engine::mupdf
