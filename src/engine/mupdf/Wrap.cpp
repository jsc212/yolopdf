// SPDX-License-Identifier: AGPL-3.0-only
#include "Lines.h"
#include <algorithm>
#include <cmath>
namespace yolo::engine::mupdf {
std::vector<ExpectedSegment> writeWrappedLine(fz_context *ctx, pdf_document *doc, int page,
                                              const Trace &trace, const EditableLine &line,
                                              const TextReplacement &edit) {
    if (line.status == TextStatus::Unsupported)
        throw Error(ErrorCode::Unsupported, line.reason);
    if (line.segments.size() != 1)
        throw Error(
            ErrorCode::Unsupported,
            "Wrapping mixed style lines is not supported yet; widen or choose a smaller size");
    const auto &run = trace.runs[line.segments[0].runId];
    const auto display = fz_concat(run.start, matrix(run.info.ctm));
    if (!run.forms.empty() || run.info.renderMode != 0 || std::abs(display.b) > .001 ||
        std::abs(display.c) > .001 || display.a <= 0 || display.d >= 0)
        throw Error(ErrorCode::Unsupported,
                    "Wrap requires horizontal filled text outside existing Forms");
    if (!std::isfinite(edit.wrapWidth) || edit.wrapWidth <= 0)
        throw Error(ErrorCode::InvalidArgument, "Choose a positive wrap width");
    const auto chars = editCharacters(edit.text);
    if (chars.empty() || chars.size() > 2048)
        throw Error(ErrorCode::InvalidArgument, "Wrap 1–2048 characters");
    auto part = edit;
    if (edit.fontSize != 0)
        part.fontSize = edit.fontSize / std::abs(display.d);
    auto encoded = encodeReplacement(ctx, doc, run, part);
    const std::size_t stride = encoded.bytes.size() / chars.size();
    if ((stride != 1 && stride != 2) || stride * chars.size() != encoded.bytes.size())
        throw Error(ErrorCode::Unsupported, "Ambiguous font encoding");
    Ref<pdf_font_desc, pdf_drop_font> font(
        ctx, checked(ctx, [&] { return pdf_load_font(ctx, doc, nullptr, encoded.font.get()); }));
    struct Glyph {
        int cp;
        std::string bytes;
        double advance;
    };
    std::vector<Glyph> glyphs;
    for (std::size_t i = 0; i < chars.size(); ++i) {
        const auto bytes = encoded.bytes.substr(i * stride, stride);
        const int code = stride == 1 ? static_cast<unsigned char>(bytes[0])
                                     : (static_cast<unsigned char>(bytes[0]) << 8) |
                                           static_cast<unsigned char>(bytes[1]);
        const int cid = pdf_lookup_cmap(font.get()->encoding, code);
        const auto metric = checked(ctx, [&] { return pdf_lookup_hmtx(ctx, font.get(), cid); });
        double advance =
            (metric.w * .001 * encoded.size + run.textState.char_space) * run.textState.scale;
        if (stride == 1 && code == 32)
            advance += run.textState.word_space * run.textState.scale;
        advance *= display.a;
        if (advance > edit.wrapWidth + .01)
            throw Error(ErrorCode::Overflow, "A glyph is wider than the wrap box");
        glyphs.push_back({chars[i], bytes, advance});
    }
    std::vector<std::vector<Glyph>> rows(1);
    double width = 0;
    for (auto &g : glyphs) {
        while (!rows.back().empty() && width + g.advance > edit.wrapWidth + .01) {
            auto &row = rows.back();
            int split = -1;
            for (int i = 0; i < int(row.size()) - 1; ++i)
                if (row[i].cp == 32)
                    split = i + 1;
            std::vector<Glyph> next;
            if (split > 0) {
                next.assign(row.begin() + split, row.end());
                row.resize(split);
            }
            rows.push_back(std::move(next));
            width = 0;
            for (auto &carry : rows.back())
                width += carry.advance;
        }
        rows.back().push_back(g);
        width += g.advance;
    }
    const double leading = encoded.size * std::abs(display.d) * 1.2;
    Page loaded(ctx, checked(ctx, [&] {
                    return reinterpret_cast<fz_page *>(pdf_load_page(ctx, doc, page));
                }));
    const auto pageBounds = checked(ctx, [&] { return fz_bound_page(ctx, loaded.get()); });
    if (display.e + edit.wrapWidth > pageBounds.x1 + .01 ||
        display.f + leading * (rows.size() - 1) > pageBounds.y1)
        throw Error(ErrorCode::Overflow, "Wrapped text exceeds the visible page");
    Object resources(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 1); }));
    Object fonts(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 1); }));
    put(ctx, fonts.get(), "WrapFont", encoded.font.get());
    put(ctx, resources.get(), "Font", fonts.get());
    Buffer contents(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 1024); }));
    std::vector<ExpectedSegment> expected;
    for (std::size_t n = 0; n < rows.size(); ++n) {
        auto m = display;
        m.f += leading * n;
        std::string bytes;
        std::vector<int> characters;
        for (auto &g : rows[n]) {
            bytes += g.bytes;
            characters.push_back(g.cp);
        }
        checked(ctx, [&] {
            fz_append_printf(
                ctx, contents.get(),
                "BT /WrapFont %g Tf %g Tc %g Tw %g Tz %g Ts 0 Tr %g %g %g %g %g %g Tm ",
                encoded.size, run.textState.char_space, run.textState.word_space,
                run.textState.scale * 100, run.textState.rise, m.a, m.b, m.c, m.d, m.e, m.f);
        });
        if (edit.changeColor)
            checked(ctx, [&] {
                fz_append_printf(ctx, contents.get(), "%g %g %g rg ", edit.color[0], edit.color[1],
                                 edit.color[2]);
            });
        checked(ctx, [&] { fz_append_string(ctx, contents.get(), "<"); });
        for (unsigned char byte : bytes)
            checked(ctx, [&] { fz_append_printf(ctx, contents.get(), "%02x", byte); });
        checked(ctx, [&] { fz_append_string(ctx, contents.get(), "> Tj ET\n"); });
        expected.push_back({run.info.id + int(n),
                            editString(characters),
                            edit.fontFile.empty() ? run.info.fontName : std::string(),
                            {m.e, m.f}});
    }
    Object form(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 5); }));
    const auto inverse = fz_invert_matrix(matrix(run.info.ctm));
    checked(ctx, [&] {
        pdf_dict_put_name(ctx, form.get(), PDF_NAME(Type), "XObject");
        pdf_dict_put_name(ctx, form.get(), PDF_NAME(Subtype), "Form");
        pdf_dict_put_rect(ctx, form.get(), PDF_NAME(BBox), pageBounds);
        pdf_dict_put_matrix(ctx, form.get(), PDF_NAME(Matrix), inverse);
    });
    put(ctx, form.get(), "Resources", resources.get());
    Object object(
        ctx, checked(ctx, [&] { return pdf_add_stream(ctx, doc, contents.get(), form.get(), 0); }));
    replaceShowWithForm(ctx, doc, page, run, object.get());
    return expected;
}
} // namespace yolo::engine::mupdf
