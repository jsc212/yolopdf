// SPDX-License-Identifier: AGPL-3.0-only
#include "Content.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ft2build.h>
#include <map>
#include <set>
#include <sstream>
#include FT_FREETYPE_H
#include <mupdf/ucdn.h>

namespace yolo::engine::mupdf {
namespace {
std::string objectName(fz_context *ctx, pdf_obj *obj) {
    return checked(ctx, [&] { return pdf_to_name(ctx, obj); });
}
std::string utf8(int code) {
    char bytes[8];
    return {bytes, static_cast<std::size_t>(fz_runetochar(bytes, code))};
}
std::vector<int> codepoints(const std::string &value) {
    std::vector<int> result;
    for (std::size_t i = 0; i < value.size();) {
        int code;
        const int n = fz_chartorune(&code, value.c_str() + i);
        if (n <= 0 || i + n > value.size() || utf8(code) != value.substr(i, n) || code < 32 ||
            code == 127 || code == 0xfffd)
            throw Error(ErrorCode::InvalidArgument, "Replacement must be valid single-line UTF-8");
        result.push_back(code);
        i += n;
    }
    return result;
}
bool axisAligned(fz_matrix m) {
    return (m.b == 0 && m.c == 0) || (m.a == 0 && m.d == 0);
}
bool complexScript(int c) {
    const int category = ucdn_get_general_category(c), script = ucdn_get_script(c);
    if (category == UCDN_GENERAL_CATEGORY_MN || category == UCDN_GENERAL_CATEGORY_MC ||
        category == UCDN_GENERAL_CATEGORY_ME || (c >= 0x200c && c <= 0x200f) ||
        (c >= 0x1100 && c <= 0x11ff) || (c >= 0xa960 && c <= 0xa97f) ||
        (c >= 0xd7b0 && c <= 0xd7ff))
        return true;
    return script != UCDN_SCRIPT_COMMON && script != UCDN_SCRIPT_LATIN &&
           script != UCDN_SCRIPT_GREEK && script != UCDN_SCRIPT_CYRILLIC &&
           script != UCDN_SCRIPT_HAN && script != UCDN_SCRIPT_HIRAGANA &&
           script != UCDN_SCRIPT_KATAKANA && script != UCDN_SCRIPT_HANGUL;
}
int unicode(pdf_font_desc *font, int cid) {
    int codes[PDF_MRANGE_CAP]{};
    const int count = font->to_unicode ? pdf_lookup_cmap_full(font->to_unicode, cid, codes) : 0;
    if (count == 1 && codes[0] != 0)
        return codes[0];
    if (count > 1)
        return -1; // Ligatures and many-to-one mappings need a separate editing model.
    if (cid >= 0 && static_cast<std::size_t>(cid) < font->cid_to_ucs_len && font->cid_to_ucs[cid])
        return font->cid_to_ucs[cid];
    return -1;
}
struct Graphics {
    fz_matrix ctm{fz_identity};
    pdf_text_state text{};
    pdf_obj *fontObject{};
    pdf_obj *gsFont{};
    std::string fontName;
    std::string fontResource;
    bool complexClip{};
    fz_rect clipBounds{fz_infinite_rect};
    std::string colorSpace{"DeviceGray"};
    std::vector<double> color{0};
    Graphics() { text.scale = 1; }
};
struct State {
    fz_context *ctx;
    pdf_document *doc;
    Trace *trace;
    Graphics graphics;
    std::vector<Graphics> stack;
    // Marked-content sequences are independent of the q/Q graphics-state stack.
    std::string markedReason;
    std::vector<std::string> markedStack;
    bool pathPresent{}, rectangularPath{}, pendingClip{};
    fz_rect pathBounds{fz_empty_rect};
    pdf_text_object_state tos{};
    std::vector<FormStep> forms;
    int showIndex{}, occurrence{};
    State(fz_context *c, pdf_document *d, Trace *t) : ctx(c), doc(d), trace(t) {
        tos.tm = tos.tlm = fz_identity;
    }
};
struct Tracer {
    pdf_processor super;
    State *state;
};
State &state(pdf_processor *p) {
    return *reinterpret_cast<Tracer *>(p)->state;
}

// Exceptions from C++ callback work must unwind before returning into MuPDF's
// interpreter. FZ_ERROR_ABORT is propagated, rather than swallowed as PDF syntax.
template <class F> void callback(fz_context *ctx, F function) {
    char message[512]{};
    try {
        function();
        return;
    } catch (const std::exception &error) {
        std::snprintf(message, sizeof(message), "%s", error.what());
    } catch (...) {
        std::snprintf(message, sizeof(message), "Content processor callback failed");
    }
    fz_throw(ctx, FZ_ERROR_ABORT, "%s", message);
}

void showBytes(State &s, TracedRun &run, const unsigned char *bytes, std::size_t size) {
    auto *font = s.graphics.text.font;
    if (!font) {
        run.info.reason = "Font and size are not set";
        return;
    }
    const unsigned char *end = bytes + size;
    while (bytes < end) {
        unsigned code;
        const int width = pdf_decode_cmap(font->encoding, const_cast<unsigned char *>(bytes),
                                          const_cast<unsigned char *>(end), &code);
        if (width <= 0 || bytes + width > end)
            throw Error(ErrorCode::InvalidDocument, "Invalid encoded glyph");
        bytes += width;
        const int cid = pdf_lookup_cmap(font->encoding, code);
        if (cid < 0) {
            run.info.reason = "Unmapped character code";
            continue;
        }
        const int ucs = unicode(font, cid);
        if (ucs < 0)
            run.info.reason = "No unambiguous Unicode mapping (or ligature)";
        else {
            run.info.text += utf8(ucs);
            if (complexScript(ucs))
                run.info.reason = "Complex shaping script or combining sequence";
        }
        fz_matrix trm;
        float advance;
        checked(s.ctx, [&] {
            pdf_tos_make_trm(s.ctx, &s.tos, &s.graphics.text, font, cid, &trm, &advance);
        });
        const auto glyphBounds = fz_transform_rect(s.tos.char_bbox, s.graphics.ctm);
        run.glyphs.push_back({ucs, fz_transform_point({trm.e, trm.f}, s.graphics.ctm)});
        run.info.bounds = rect(fz_union_rect(rect(run.info.bounds), glyphBounds));
        const auto charAdvance = s.tos.char_tx;
        pdf_tos_move_after_char(s.ctx, &s.tos); // Pure state/matrix operations; no allocations.
        run.advance += charAdvance;
        if (code == 32 && width == 1) {
            const float extra = s.graphics.text.word_space * s.graphics.text.scale;
            s.tos.tm = fz_pre_translate(s.tos.tm, extra, 0);
            run.advance += extra;
        }
    }
}
void textShow(State &s, const char *bytes, std::size_t size, pdf_obj *array = nullptr) {
    if (array) {
        const int count = checked(s.ctx, [&] { return pdf_array_len(s.ctx, array); });
        bool strings = false;
        for (int i = 0; i < count; ++i)
            strings = strings || checked(s.ctx, [&] {
                          return pdf_is_string(s.ctx, pdf_array_get(s.ctx, array, i));
                      });
        if (!strings) {
            for (int i = 0; i < count; ++i) {
                const float value = checked(
                    s.ctx, [&] { return pdf_to_real(s.ctx, pdf_array_get(s.ctx, array, i)); });
                s.tos.tm = fz_pre_translate(
                    s.tos.tm, -value * s.graphics.text.size * .001f * s.graphics.text.scale, 0);
            }
            return;
        }
    }
    TracedRun run;
    run.info.id = static_cast<int>(s.trace->runs.size());
    run.showIndex = s.showIndex++;
    run.forms = s.forms;
    run.font = s.graphics.text.font;
    run.fontObject = s.graphics.fontObject;
    run.fontResource = s.graphics.fontResource;
    run.textState = s.graphics.text;
    run.start = s.tos.tm;
    run.lineMatrix = s.tos.tlm;
    run.info.textMatrix = matrix(run.start);
    run.info.ctm = matrix(s.graphics.ctm);
    run.info.fontSize = s.graphics.text.size;
    run.info.fontName = s.graphics.fontName;
    run.info.charSpacing = s.graphics.text.char_space;
    run.info.wordSpacing = s.graphics.text.word_space;
    run.info.horizontalScale = s.graphics.text.scale * 100;
    run.info.rise = s.graphics.text.rise;
    run.info.renderMode = s.graphics.text.render;
    run.info.colorSpace = s.graphics.colorSpace;
    run.info.color = s.graphics.color;
    run.info.bounds = rect(fz_empty_rect);
    if (array) {
        const int count = checked(s.ctx, [&] { return pdf_array_len(s.ctx, array); });
        for (int i = 0; i < count; ++i) {
            auto *item = checked(s.ctx, [&] { return pdf_array_get(s.ctx, array, i); });
            if (checked(s.ctx, [&] { return pdf_is_string(s.ctx, item); })) {
                auto *str = checked(s.ctx, [&] { return pdf_to_str_buf(s.ctx, item); });
                const auto len = checked(s.ctx, [&] { return pdf_to_str_len(s.ctx, item); });
                showBytes(s, run, reinterpret_cast<const unsigned char *>(str), len);
            } else {
                const float adjustment = checked(s.ctx, [&] { return pdf_to_real(s.ctx, item); });
                const float delta =
                    -adjustment * s.graphics.text.size * .001f * s.graphics.text.scale;
                s.tos.tm = fz_pre_translate(s.tos.tm, delta, 0);
                run.advance += delta;
            }
        }
    } else
        showBytes(s, run, reinterpret_cast<const unsigned char *>(bytes), size);
    run.end = s.tos.tm;
    run.clipSafe =
        !s.graphics.complexClip && fz_contains_rect(s.graphics.clipBounds, rect(run.info.bounds));
    if (!run.font)
        run.info.reason = "No font";
    else if (!run.fontObject)
        run.info.reason = "Font resource could not be resolved";
    else if (std::any_of(run.forms.begin(), run.forms.end(), [&](const auto &step) {
                 return get(s.ctx, step.form, "StructParents") != nullptr;
             }))
        run.info.reason = "Tagged Form editing requires structure-tree remapping";
    else if (run.font->wmode)
        run.info.reason = "Vertical writing mode";
    else if (run.font->font->t3procs)
        run.info.reason = "Type3 font";
    else if (run.info.renderMode >= 3)
        run.info.reason = "Invisible or clipping text render mode";
    else if (s.graphics.complexClip ||
             !fz_contains_rect(s.graphics.clipBounds, rect(run.info.bounds)))
        run.info.reason = "Text under a path clipping operation";
    else if (!s.markedReason.empty())
        run.info.reason = s.markedReason;
    else if (s.graphics.text.size <= 0 || s.graphics.text.scale <= 0)
        run.info.reason = "Non-positive font size or horizontal scale";
    else if (objectName(s.ctx, get(s.ctx, run.fontObject, "Subtype")) == "Type0" &&
             objectName(s.ctx, get(s.ctx, run.fontObject, "Encoding")) != "Identity-H")
        run.info.reason = "Non-Identity-H composite encoding is not editable in this spike";
    if (run.info.reason.empty()) {
        static const std::set<std::string> standardFonts = {
            "Helvetica",   "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
            "Times-Roman", "Times-Bold",     "Times-Italic",      "Times-BoldItalic",
            "Courier",     "Courier-Bold",   "Courier-Oblique",   "Courier-BoldOblique",
            "Symbol",      "ZapfDingbats"};
        if (!run.font->is_embedded && !standardFonts.count(run.info.fontName)) {
            run.info.status = TextStatus::NeedsFont;
            run.info.reason = "Non-embedded font needs an explicit exact match or import";
        } else {
            run.info.status = TextStatus::OriginalFont;
            if (!run.font->is_embedded) {
                // Independent viewers use different standard-font programs.
                // Include their glyph overhang in the declared edit rectangle.
                const float margin = .25f * run.textState.size * fz_matrix_expansion(run.start) *
                                     fz_matrix_expansion(s.graphics.ctm);
                run.info.bounds = rect(fz_expand_rect(rect(run.info.bounds), margin));
            }
        }
    }
    s.trace->runs.push_back(std::move(run));
}

void process(State &, pdf_obj *, pdf_obj *);
pdf_processor *newTracer(fz_context *ctx, State &s) {
    auto *tracer = static_cast<Tracer *>(pdf_new_processor(ctx, sizeof(Tracer)));
    auto *p = &tracer->super;
    tracer->state = &s;
    p->requirements = PDF_PROCESSOR_REQUIRES_DECODED_IMAGES;
    p->drop_processor = nullptr;
    p->op_q = [](fz_context *c, pdf_processor *p) {
        callback(c, [&] {
            auto &s = state(p);
            s.stack.push_back(s.graphics);
        });
    };
    p->op_Q = [](fz_context *c, pdf_processor *p) {
        callback(c, [&] {
            auto &s = state(p);
            if (s.stack.empty())
                throw Error(ErrorCode::InvalidDocument, "Unbalanced graphics state");
            s.graphics = s.stack.back();
            s.stack.pop_back();
        });
    };
    p->op_cm = [](fz_context *, pdf_processor *p, float a, float b, float c, float d, float e,
                  float f) {
        auto &s = state(p);
        s.graphics.ctm = fz_concat({a, b, c, d, e, f}, s.graphics.ctm);
    };
    p->op_BT = [](fz_context *, pdf_processor *p) {
        auto &s = state(p);
        s.tos.tm = s.tos.tlm = fz_identity;
    };
    p->op_Tf = [](fz_context *c, pdf_processor *p, const char *n, pdf_font_desc *font, float size) {
        callback(c, [&] {
            auto &s = state(p);
            s.graphics.text.font = font;
            s.graphics.text.size = size;
            s.graphics.fontObject =
                s.graphics.gsFont ? s.graphics.gsFont
                : n               ? checked(c,
                                            [&] { return pdf_lookup_resource(c, p->rstack, PDF_NAME(Font), n); })
                                  : nullptr;
            s.graphics.fontName = objectName(c, get(c, s.graphics.fontObject, "BaseFont"));
            s.graphics.fontResource = n ? n : "";
            if (std::find(s.trace->fonts.begin(), s.trace->fonts.end(), font) ==
                s.trace->fonts.end()) {
                s.trace->fonts.push_back(font);
                pdf_keep_font(c, font);
            }
        });
    };
    p->op_gs_begin = [](fz_context *c, pdf_processor *p, const char *, pdf_obj *object) {
        callback(c, [&] {
            auto *font = get(c, object, "Font");
            state(p).graphics.gsFont = checked(c, [&] { return pdf_array_get(c, font, 0); });
        });
    };
    p->op_gs_end = [](fz_context *, pdf_processor *p) { state(p).graphics.gsFont = nullptr; };
    p->op_Tc = [](fz_context *, pdf_processor *p, float v) {
        state(p).graphics.text.char_space = v;
    };
    p->op_Tw = [](fz_context *, pdf_processor *p, float v) {
        state(p).graphics.text.word_space = v;
    };
    p->op_Tz = [](fz_context *, pdf_processor *p, float v) {
        state(p).graphics.text.scale = v / 100;
    };
    p->op_TL = [](fz_context *, pdf_processor *p, float v) { state(p).graphics.text.leading = v; };
    p->op_Ts = [](fz_context *, pdf_processor *p, float v) { state(p).graphics.text.rise = v; };
    p->op_Tr = [](fz_context *, pdf_processor *p, int v) { state(p).graphics.text.render = v; };
    p->op_Tm = [](fz_context *, pdf_processor *p, float a, float b, float c, float d, float e,
                  float f) { pdf_tos_set_matrix(&state(p).tos, a, b, c, d, e, f); };
    p->op_Td = [](fz_context *, pdf_processor *p, float x, float y) {
        pdf_tos_translate(&state(p).tos, x, y);
    };
    p->op_TD = [](fz_context *, pdf_processor *p, float x, float y) {
        auto &s = state(p);
        s.graphics.text.leading = -y;
        pdf_tos_translate(&s.tos, x, y);
    };
    p->op_Tstar = [](fz_context *, pdf_processor *p) {
        auto &s = state(p);
        pdf_tos_newline(&s.tos, s.graphics.text.leading);
    };
    p->op_Tj = [](fz_context *c, pdf_processor *p, char *v, std::size_t n) {
        callback(c, [&] { textShow(state(p), v, n); });
    };
    p->op_TJ = [](fz_context *c, pdf_processor *p, pdf_obj *a) {
        callback(c, [&] { textShow(state(p), nullptr, 0, a); });
    };
    p->op_squote = [](fz_context *c, pdf_processor *p, char *v, std::size_t n) {
        callback(c, [&] {
            auto &s = state(p);
            pdf_tos_newline(&s.tos, s.graphics.text.leading);
            textShow(s, v, n);
        });
    };
    p->op_dquote = [](fz_context *c, pdf_processor *p, float w, float a, char *v, std::size_t n) {
        callback(c, [&] {
            auto &s = state(p);
            s.graphics.text.word_space = w;
            s.graphics.text.char_space = a;
            pdf_tos_newline(&s.tos, s.graphics.text.leading);
            textShow(s, v, n);
        });
    };
    p->op_m = p->op_l = [](fz_context *, pdf_processor *p, float, float) {
        state(p).pathPresent = true;
        state(p).rectangularPath = false;
    };
    p->op_c = [](fz_context *, pdf_processor *p, float, float, float, float, float, float) {
        state(p).pathPresent = true;
        state(p).rectangularPath = false;
    };
    p->op_v = p->op_y = [](fz_context *, pdf_processor *p, float, float, float, float) {
        state(p).pathPresent = true;
        state(p).rectangularPath = false;
    };
    p->op_re = [](fz_context *, pdf_processor *p, float x, float y, float w, float h) {
        auto &s = state(p);
        const auto m = s.graphics.ctm;
        s.rectangularPath = !s.pathPresent && axisAligned(m);
        s.pathPresent = true;
        s.pathBounds = fz_transform_rect(
            {std::min(x, x + w), std::min(y, y + h), std::max(x, x + w), std::max(y, y + h)}, m);
    };
    p->op_W = p->op_Wstar = [](fz_context *, pdf_processor *p) { state(p).pendingClip = true; };
    auto endPath = [](fz_context *, pdf_processor *p) {
        auto &s = state(p);
        if (s.pendingClip) {
            if (s.rectangularPath)
                s.graphics.clipBounds = fz_intersect_rect(s.graphics.clipBounds, s.pathBounds);
            else
                s.graphics.complexClip = true;
        }
        s.pathPresent = s.rectangularPath = s.pendingClip = false;
    };
    p->op_n = p->op_S = p->op_s = p->op_F = p->op_f = p->op_fstar = p->op_B = p->op_Bstar =
        p->op_b = p->op_bstar = endPath;
    p->op_BMC = [](fz_context *c, pdf_processor *p, const char *) {
        callback(c, [&] {
            auto &s = state(p);
            s.markedStack.push_back(s.markedReason);
        });
    };
    p->op_BDC = [](fz_context *c, pdf_processor *p, const char *, pdf_obj *, pdf_obj *properties) {
        callback(c, [&] {
            auto &s = state(p);
            s.markedStack.push_back(s.markedReason);
            if (get(c, properties, "ActualText"))
                s.markedReason = "Marked content has an ActualText override";
        });
    };
    p->op_EMC = [](fz_context *c, pdf_processor *p) {
        callback(c, [&] {
            auto &s = state(p);
            if (s.markedStack.empty())
                throw Error(ErrorCode::InvalidDocument, "Unbalanced marked content");
            s.markedReason = s.markedStack.back();
            s.markedStack.pop_back();
        });
    };
    p->op_g = [](fz_context *c, pdf_processor *p, float v) {
        callback(c, [&] {
            auto &g = state(p).graphics;
            g.colorSpace = "DeviceGray";
            g.color = {v};
        });
    };
    p->op_rg = [](fz_context *c, pdf_processor *p, float r, float g, float b) {
        callback(c, [&] {
            auto &s = state(p).graphics;
            s.colorSpace = "DeviceRGB";
            s.color = {r, g, b};
        });
    };
    p->op_k = [](fz_context *c, pdf_processor *p, float a, float b, float d, float e) {
        callback(c, [&] {
            auto &g = state(p).graphics;
            g.colorSpace = "DeviceCMYK";
            g.color = {a, b, d, e};
        });
    };
    p->op_cs = [](fz_context *c, pdf_processor *p, const char *name, fz_colorspace *) {
        callback(c, [&] { state(p).graphics.colorSpace = name; });
    };
    p->op_sc_color = [](fz_context *c, pdf_processor *p, int n, float *v) {
        callback(c, [&] { state(p).graphics.color.assign(v, v + n); });
    };
    p->op_sc_pattern = [](fz_context *c, pdf_processor *p, const char *n, pdf_pattern *, int count,
                          float *v) {
        callback(c, [&] {
            auto &g = state(p).graphics;
            g.colorSpace = std::string("Pattern/") + n;
            g.color.assign(v, v + count);
        });
    };
    p->op_Do_form = [](fz_context *c, pdf_processor *p, const char *, pdf_obj *form) {
        callback(c, [&] {
            auto &s = state(p);
            const int occurrence = s.occurrence++;
            if (s.forms.size() >= 64 || std::any_of(s.forms.begin(), s.forms.end(),
                                                    [&](auto step) { return step.form == form; }))
                throw Error(ErrorCode::InvalidDocument,
                            "Recursive or excessively nested Form content");
            State child(c, s.doc, s.trace);
            child.graphics = s.graphics;
            // A Form has its own sequence stack but inherits outer restrictions.
            child.markedReason = s.markedReason;
            child.forms = s.forms;
            child.forms.push_back({occurrence, form});
            auto *transform = get(c, form, "Matrix");
            if (transform)
                child.graphics.ctm = fz_concat(
                    checked(c, [&] { return pdf_to_matrix(c, transform); }), child.graphics.ctm);
            if (auto *box = get(c, form, "BBox")) {
                if (axisAligned(child.graphics.ctm))
                    child.graphics.clipBounds = fz_intersect_rect(
                        child.graphics.clipBounds,
                        fz_transform_rect(checked(c, [&] { return pdf_to_rect(c, box); }),
                                          child.graphics.ctm));
                else
                    child.graphics.complexClip = true;
            }
            auto *resources = get(c, form, "Resources");
            if (!resources && p->rstack)
                resources = p->rstack->resources;
            process(child, resources, form);
        });
    };
    auto recordImage = [](fz_context *c, pdf_processor *p, fz_image *image, bool inlined) {
        callback(c, [&] {
            auto &s = state(p);
            TracedImage traced;
            traced.info.id = static_cast<int>(s.trace->images.size());
            traced.info.inlineImage = inlined;
            traced.info.width = image->w;
            traced.info.height = image->h;
            traced.ctm = s.graphics.ctm;
            traced.info.transform = matrix(traced.ctm);
            traced.info.bounds = rect(fz_transform_rect(fz_unit_rect, traced.ctm));
            traced.occurrence = s.occurrence++;
            traced.forms = s.forms;
            traced.image = image;
            s.trace->images.push_back(std::move(traced));
            fz_keep_image(c, image);
        });
    };
    // Store the non-capturing function pointer once for both callbacks.
    static auto imageRecorder = recordImage;
    p->op_Do_image = [](fz_context *c, pdf_processor *p, const char *name, fz_image *image) {
        imageRecorder(c, p, image, false);
        callback(c, [&] {
            state(p).trace->images.back().object = checked(
                c, [&] { return pdf_lookup_resource(c, p->rstack, PDF_NAME(XObject), name); });
        });
    };
    p->op_BI = [](fz_context *c, pdf_processor *p, fz_image *image, const char *) {
        imageRecorder(c, p, image, true);
    };
    return p;
}
void process(State &s, pdf_obj *resources, pdf_obj *contents) {
    Processor proc(s.ctx, checked(s.ctx, [&] { return newTracer(s.ctx, s); }));
    checked(s.ctx, [&] {
        pdf_process_contents(s.ctx, proc.get(), s.doc, resources, contents, nullptr, nullptr);
    });
    checked(s.ctx, [&] { pdf_close_processor(s.ctx, proc.get()); });
}

struct Rewrite {
    Rewrite(fz_context *c, pdf_document *d, const std::vector<FormStep> *f)
        : ctx(c), doc(d), forms(f) {}
    fz_context *ctx;
    pdf_document *doc;
    const std::vector<FormStep> *forms;
    std::size_t depth{};
    int show{}, occurrence{};
    int targetShow{-1}, targetImage{-1};
    pdf_obj *replacementArray{};
    pdf_obj *resources{};
    std::string replacementName;
    pdf_obj *replacementImage{};
    pdf_obj *replacementFont{};
    pdf_obj *originalFont{};
    std::string replacementFontName, originalFontName;
    float fontSize{}, originalFontSize{};
    pdf_obj *shiftBefore{}, *shiftAfter{};
    pdf_obj *wrappedForm{};
    std::string wrappedName;
    fz_matrix originalLineMatrix{};
    bool changeColor{};
    std::array<double, 3> color{};
    ImageAction imageAction{};
    fz_matrix imageDelta{fz_identity};
    bool matched{};
};
struct Rewriter {
    pdf_processor super;
    Rewrite *edit;
};
Rewrite &edit(pdf_processor *p) {
    return *reinterpret_cast<Rewriter *>(p)->edit;
}
void forwardText(fz_context *ctx, pdf_processor *p, char *bytes, std::size_t n, pdf_obj *array,
                 int quote = 0, float w = 0, float c = 0) {
    auto &e = edit(p);
    bool strings = true;
    if (array) {
        strings = false;
        const int count = checked(ctx, [&] { return pdf_array_len(ctx, array); });
        for (int i = 0; i < count; ++i)
            strings = strings || checked(ctx, [&] {
                          return pdf_is_string(ctx, pdf_array_get(ctx, array, i));
                      });
    }
    const bool selected = strings && e.depth == e.forms->size() && e.show++ == e.targetShow;
    checked(ctx, [&] {
        auto *out = p->chain;
        if (selected) {
            if (quote == 2) {
                out->op_Tw(ctx, out, w);
                out->op_Tc(ctx, out, c);
            }
            if (quote)
                out->op_Tstar(ctx, out);
            if (e.changeColor && !e.wrappedForm) {
                out->op_q(ctx, out);
                out->op_rg(ctx, out, e.color[0], e.color[1], e.color[2]);
                out->op_RG(ctx, out, e.color[0], e.color[1], e.color[2]);
            }
            if (e.wrappedForm) {
                out->op_ET(ctx, out);
                out->op_q(ctx, out);
                out->op_Do_form(ctx, out, e.wrappedName.c_str(), e.wrappedForm);
                out->op_Q(ctx, out);
                out->op_BT(ctx, out);
                out->op_Tf(ctx, out, e.originalFontName.c_str(), nullptr, e.originalFontSize);
                auto m = e.originalLineMatrix;
                out->op_Tm(ctx, out, m.a, m.b, m.c, m.d, m.e, m.f);
                out->op_TJ(ctx, out, e.shiftAfter);
                return;
            }
            if (e.replacementFont)
                out->op_Tf(ctx, out, e.replacementFontName.c_str(), nullptr, e.fontSize);
            if (e.shiftBefore)
                out->op_TJ(ctx, out, e.shiftBefore);
            out->op_TJ(ctx, out, e.replacementArray);
            if (e.shiftAfter)
                out->op_TJ(ctx, out, e.shiftAfter);
            if (e.replacementFont)
                out->op_Tf(ctx, out, e.originalFontName.c_str(), nullptr, e.originalFontSize);
            if (e.changeColor)
                out->op_Q(ctx, out);
        } else if (quote == 2)
            out->op_dquote(ctx, out, w, c, bytes, n);
        else if (quote == 1)
            out->op_squote(ctx, out, bytes, n);
        else if (array)
            out->op_TJ(ctx, out, array);
        else
            out->op_Tj(ctx, out, bytes, n);
    });
    if (selected)
        e.matched = true;
}
// Clone each resource subdictionary that editing could modify. Font programs,
// images, and all unselected content streams remain shared and untouched.
pdf_obj *copyResources(fz_context *ctx, pdf_document *doc, pdf_obj *resources) {
    Object copy(ctx, checked(ctx, [&] {
                    return resources ? pdf_copy_dict(ctx, resources) : pdf_new_dict(ctx, doc, 4);
                }));
    for (const char *key : {"XObject", "Font"}) {
        auto *old = get(ctx, copy.get(), key);
        Object value(ctx, checked(ctx, [&] {
                         return old ? pdf_copy_dict(ctx, old) : pdf_new_dict(ctx, doc, 4);
                     }));
        put(ctx, copy.get(), key, value.get());
    }
    return pdf_keep_obj(ctx, copy.get());
}
std::string addResource(fz_context *ctx, pdf_obj *resources, const char *type, pdf_obj *object) {
    auto *dict = get(ctx, resources, type);
    for (int i = 0;; ++i) {
        const auto name = "Yolo" + std::to_string(i);
        if (!get(ctx, dict, name.c_str())) {
            put(ctx, dict, name.c_str(), object);
            return name;
        }
    }
}
fz_buffer *rewriteStream(Rewrite &, pdf_obj *, pdf_obj *);
pdf_processor *newRewriter(fz_context *ctx, Rewrite &e, pdf_processor *chain) {
    auto *proc = static_cast<Rewriter *>(pdf_new_processor(ctx, sizeof(Rewriter)));
    auto *p = &proc->super;
    proc->edit = &e;
    p->chain = chain;
    p->drop_processor = nullptr;
    p->requirements = PDF_PROCESSOR_REQUIRES_DECODED_IMAGES;
    p->op_Tj = [](fz_context *c, pdf_processor *p, char *b, std::size_t n) {
        callback(c, [&] { forwardText(c, p, b, n, nullptr); });
    };
    p->op_TJ = [](fz_context *c, pdf_processor *p, pdf_obj *a) {
        callback(c, [&] { forwardText(c, p, nullptr, 0, a); });
    };
    p->op_squote = [](fz_context *c, pdf_processor *p, char *b, std::size_t n) {
        callback(c, [&] { forwardText(c, p, b, n, nullptr, 1); });
    };
    p->op_dquote = [](fz_context *c, pdf_processor *p, float w, float a, char *b, std::size_t n) {
        callback(c, [&] { forwardText(c, p, b, n, nullptr, 2, w, a); });
    };
    p->op_Do_form = [](fz_context *c, pdf_processor *p, const char *name, pdf_obj *form) {
        callback(c, [&] {
            auto &e = edit(p);
            const int occurrence = e.occurrence++;
            if (e.depth < e.forms->size() && occurrence == (*e.forms)[e.depth].occurrence) {
                Object res(c, copyResources(c, e.doc,
                                            get(c, form, "Resources") ? get(c, form, "Resources")
                                                                      : e.resources));
                Rewrite child = e;
                child.depth++;
                child.show = child.occurrence = 0;
                child.resources = res.get();
                child.matched = false;
                if (child.replacementImage)
                    child.replacementName =
                        addResource(c, res.get(), "XObject", child.replacementImage);
                if (child.replacementFont)
                    child.replacementFontName =
                        addResource(c, res.get(), "Font", child.replacementFont);
                if (child.originalFont &&
                    !get(c, get(c, res.get(), "Font"), child.originalFontName.c_str()))
                    child.originalFontName = addResource(c, res.get(), "Font", child.originalFont);
                Buffer buffer(c, rewriteStream(child, res.get(), form));
                Object dict(c, checked(c, [&] { return pdf_copy_dict(c, form); }));
                put(c, dict.get(), "Resources", res.get());
                Object cloned(c, checked(c, [&] {
                                  return pdf_add_stream(c, e.doc, buffer.get(), dict.get(), 0);
                              }));
                const auto unique = addResource(c, e.resources, "XObject", cloned.get());
                checked(c,
                        [&] { p->chain->op_Do_form(c, p->chain, unique.c_str(), cloned.get()); });
                e.matched = child.matched;
            } else
                checked(c, [&] { p->chain->op_Do_form(c, p->chain, name, form); });
        });
    };
    p->op_Do_image = [](fz_context *c, pdf_processor *p, const char *name, fz_image *image) {
        callback(c, [&] {
            auto &e = edit(p);
            const int occurrence = e.occurrence++;
            const bool chosen = e.depth == e.forms->size() && occurrence == e.targetImage;
            if (!chosen) {
                checked(c, [&] { p->chain->op_Do_image(c, p->chain, name, image); });
                return;
            }
            e.matched = true;
            if (e.imageAction == ImageAction::Delete)
                return;
            checked(c, [&] {
                auto *out = p->chain;
                out->op_q(c, out);
                if (e.imageAction == ImageAction::Transform) {
                    auto m = e.imageDelta;
                    out->op_cm(c, out, m.a, m.b, m.c, m.d, m.e, m.f);
                }
                out->op_Do_image(c, out,
                                 e.imageAction == ImageAction::Replace ? e.replacementName.c_str()
                                                                       : name,
                                 image);
                out->op_Q(c, out);
            });
        });
    };
    p->op_BI = [](fz_context *c, pdf_processor *p, fz_image *image, const char *cs) {
        callback(c, [&] {
            auto &e = edit(p);
            const int occurrence = e.occurrence++;
            const bool chosen = e.depth == e.forms->size() && occurrence == e.targetImage;
            if (!chosen) {
                checked(c, [&] { p->chain->op_BI(c, p->chain, image, cs); });
                return;
            }
            e.matched = true;
            if (e.imageAction == ImageAction::Delete)
                return;
            checked(c, [&] {
                auto *out = p->chain;
                out->op_q(c, out);
                if (e.imageAction == ImageAction::Transform) {
                    auto m = e.imageDelta;
                    out->op_cm(c, out, m.a, m.b, m.c, m.d, m.e, m.f);
                    out->op_BI(c, out, image, cs);
                } else
                    out->op_Do_image(c, out, e.replacementName.c_str(), image);
                out->op_Q(c, out);
            });
        });
    };
    return p;
}
fz_buffer *rewriteStream(Rewrite &e, pdf_obj *resources, pdf_obj *contents) {
    Buffer buffer(e.ctx, checked(e.ctx, [&] { return fz_new_buffer(e.ctx, 1024); }));
    Processor out(
        e.ctx, checked(e.ctx, [&] { return pdf_new_buffer_processor(e.ctx, buffer.get(), 1, 1); }));
    Processor proc(e.ctx, checked(e.ctx, [&] { return newRewriter(e.ctx, e, out.get()); }));
    checked(e.ctx, [&] {
        pdf_process_contents(e.ctx, proc.get(), e.doc, resources, contents, nullptr, nullptr);
        pdf_close_processor(e.ctx, proc.get());
    });
    return fz_keep_buffer(e.ctx, buffer.get());
}
// Preserve untouched operator bytes. Canonicalizing an entire page rounds PDF
// numbers to MuPDF floats and can move unrelated glyphs in double-precision viewers.
bool patchDirectOperator(fz_context *ctx, pdf_document *doc, int page, Rewrite &e,
                         pdf_obj *resources) {
    if (!e.forms->empty())
        return false;
    auto *obj = pageObject(ctx, doc, page);
    auto *contents = get(ctx, obj, "Contents");
    const bool array = checked(ctx, [&] { return pdf_is_array(ctx, contents); });
    const int count = array ? checked(ctx, [&] { return pdf_array_len(ctx, contents); }) : 1;
    int show = 0, occurrence = 0;
    for (int index = 0; index < count; ++index) {
        auto *stream =
            array ? checked(ctx, [&] { return pdf_array_get(ctx, contents, index); }) : contents;
        Buffer original(ctx, checked(ctx, [&] { return pdf_load_stream(ctx, stream); }));
        Ref<fz_stream, fz_drop_stream> input(
            ctx, checked(ctx, [&] { return fz_open_buffer(ctx, original.get()); }));
        struct Lexer {
            fz_context *ctx;
            pdf_lexbuf buffer;
            Lexer(fz_context *c) : ctx(c) { pdf_lexbuf_init(ctx, &buffer, PDF_LEXBUF_SMALL); }
            ~Lexer() { pdf_lexbuf_fin(ctx, &buffer); }
        } lexer(ctx);
        int depth = 0;
        std::int64_t start = -1;
        bool strings = false;
        std::string name;
        std::vector<float> numbers;
        while (true) {
            const auto at = checked(ctx, [&] { return fz_tell(ctx, input.get()); });
            const auto token =
                checked(ctx, [&] { return pdf_lex(ctx, input.get(), &lexer.buffer); });
            if (token == PDF_TOK_EOF)
                break;
            if (start < 0)
                start = at;
            if (token == PDF_TOK_OPEN_ARRAY || token == PDF_TOK_OPEN_DICT) {
                ++depth;
                continue;
            }
            if (token == PDF_TOK_CLOSE_ARRAY || token == PDF_TOK_CLOSE_DICT) {
                --depth;
                continue;
            }
            if (token == PDF_TOK_STRING)
                strings = true;
            if (depth == 0 && token == PDF_TOK_INT)
                numbers.push_back(float(lexer.buffer.i));
            if (depth == 0 && token == PDF_TOK_REAL)
                numbers.push_back(lexer.buffer.f);
            if (token == PDF_TOK_NAME && depth == 0)
                name = lexer.buffer.scratch;
            if (token != PDF_TOK_KEYWORD || depth != 0)
                continue;
            const std::string op = lexer.buffer.scratch;
            if (op == "BI")
                return false; // Encoded inline image data requires the interpreter.
            bool selected = false;
            if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
                if (op != "TJ" || strings)
                    selected = show++ == e.targetShow;
            } else if (op == "Do") {
                auto *xobject = get(ctx, get(ctx, resources, "XObject"), name.c_str());
                const auto type = objectName(ctx, get(ctx, xobject, "Subtype"));
                if (type == "Image" || type == "Form")
                    selected = occurrence++ == e.targetImage;
            }
            if (selected) {
                Buffer replacement(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 256); }));
                Processor out(ctx, checked(ctx, [&] {
                                  return pdf_new_buffer_processor(ctx, replacement.get(), 1, 1);
                              }));
                Rewrite local = e;
                local.show = 0;
                local.occurrence = 0;
                local.targetShow = 0;
                local.targetImage = 0;
                Processor proxy(ctx,
                                checked(ctx, [&] { return newRewriter(ctx, local, out.get()); }));
                if (op == "Do") {
                    checked(ctx, [&] {
                        proxy.get()->op_Do_image(ctx, proxy.get(), name.c_str(), nullptr);
                    });
                } else {
                    if (op == "\"" && numbers.size() != 2)
                        return false;
                    forwardText(ctx, proxy.get(), nullptr, 0, e.replacementArray,
                                op == "\""  ? 2
                                : op == "'" ? 1
                                            : 0,
                                op == "\"" ? numbers[0] : 0, op == "\"" ? numbers[1] : 0);
                }
                checked(ctx, [&] { pdf_close_processor(ctx, proxy.get()); });
                if (!local.matched)
                    throw Error(ErrorCode::UnsafeEdit,
                                "Selected content occurrence no longer exists");
                unsigned char *data;
                const auto size =
                    checked(ctx, [&] { return fz_buffer_storage(ctx, original.get(), &data); });
                const auto end = checked(ctx, [&] { return fz_tell(ctx, input.get()); });
                Buffer patched(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, size + 256); }));
                checked(ctx, [&] {
                    fz_append_data(ctx, patched.get(), data, start);
                    fz_append_string(ctx, patched.get(), "\n");
                    fz_append_buffer(ctx, patched.get(), replacement.get());
                    fz_append_string(ctx, patched.get(), "\n");
                    fz_append_data(ctx, patched.get(), data + end, size - end);
                });
                Object changed(ctx, checked(ctx, [&] {
                                   return pdf_add_stream(ctx, doc, patched.get(), nullptr, 0);
                               }));
                if (array) {
                    Object copied(ctx, checked(ctx, [&] { return pdf_copy_array(ctx, contents); }));
                    checked(ctx, [&] { pdf_array_put(ctx, copied.get(), index, changed.get()); });
                    put(ctx, obj, "Contents", copied.get());
                } else
                    put(ctx, obj, "Contents", changed.get());
                put(ctx, obj, "Resources", resources);
                e.matched = local.matched;
                return e.matched;
            }
            start = -1;
            strings = false;
            name.clear();
            numbers.clear();
        }
        if (depth || start >= 0)
            return false; // Operands spanning streams use the interpreter.
    }
    return false;
}
void updatePage(fz_context *ctx, pdf_document *doc, int page, Rewrite &e, pdf_obj *resources) {
    if (patchDirectOperator(ctx, doc, page, e, resources))
        return;
    auto *obj = pageObject(ctx, doc, page);
    e.resources = resources;
    Buffer buffer(ctx, rewriteStream(e, resources, get(ctx, obj, "Contents")));
    if (!e.matched)
        throw Error(ErrorCode::UnsafeEdit, "Selected content occurrence no longer exists");
    Object stream(ctx,
                  checked(ctx, [&] { return pdf_add_stream(ctx, doc, buffer.get(), nullptr, 0); }));
    put(ctx, obj, "Contents", stream.get());
    put(ctx, obj, "Resources", resources);
}
std::pair<std::string, double> encode(fz_context *ctx, const TracedRun &run,
                                      const std::string &replacement) {
    const auto characters = codepoints(replacement);
    if (std::any_of(characters.begin(), characters.end(), complexScript))
        throw Error(ErrorCode::Unsupported, "Replacement requires complex shaping");
    const bool composite = objectName(ctx, get(ctx, run.fontObject, "Subtype")) == "Type0";
    std::map<int, int> reverse;
    const int limit = composite ? 65536 : 256;
    for (int code = 0; code < limit; ++code) {
        const int cid = pdf_lookup_cmap(run.font->encoding, code);
        if (cid < 0)
            continue;
        const int ucs = unicode(run.font, cid);
        if (ucs < 0 || reverse.count(ucs))
            continue;
        const int gid = checked(ctx, [&] { return pdf_font_cid_to_gid(ctx, run.font, cid); });
        auto *face = static_cast<FT_Face>(fz_font_ft_face(ctx, run.font->font));
        if (gid <= 0 || !face || gid >= face->num_glyphs)
            continue;
        reverse[ucs] = code;
    }
    std::string encoded;
    double advance = 0;
    for (int ucs : characters) {
        const auto found = reverse.find(ucs);
        if (found == reverse.end())
            throw Error(ErrorCode::NeedsFont,
                        "Original font cannot encode " + utf8(ucs) + "; explicitly import a font");
        const int code = found->second, cid = pdf_lookup_cmap(run.font->encoding, code);
        auto *face = static_cast<FT_Face>(fz_font_ft_face(ctx, run.font->font));
        const int gid = checked(ctx, [&] { return pdf_font_cid_to_gid(ctx, run.font, cid); });
        const int glyphError = checked(ctx, [&] {
            fz_ft_lock(ctx); // Also binds MuPDF's allocator to this context.
            const int result = FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
            fz_ft_unlock(ctx);
            return result;
        });
        if (glyphError)
            throw Error(ErrorCode::NeedsFont, "Embedded font program cannot load " + utf8(ucs));
        if (composite)
            encoded.push_back(char(code >> 8));
        encoded.push_back(char(code));
        const auto metrics = checked(ctx, [&] { return pdf_lookup_hmtx(ctx, run.font, cid); });
        advance += (metrics.w * .001 * run.textState.size + run.textState.char_space) *
                   run.textState.scale;
        if (code == 32 && !composite)
            advance += run.textState.word_space * run.textState.scale;
    }
    return {encoded, advance};
}
} // namespace

std::vector<int> editCharacters(const std::string &s) {
    return codepoints(s);
}
std::string editString(const std::vector<int> &v) {
    std::string s;
    for (int c : v)
        s += utf8(c);
    return s;
}

Trace::~Trace() {
    for (auto *f : fonts)
        pdf_drop_font(ctx, f);
    for (auto &i : images)
        fz_drop_image(ctx, i.image);
}
fz_matrix pageTransform(fz_context *ctx, pdf_document *doc, int index) {
    pageObject(ctx, doc, index);
    Page page(ctx, checked(ctx, [&] {
                  return reinterpret_cast<fz_page *>(pdf_load_page(ctx, doc, index));
              }));
    fz_matrix transform;
    fz_rect media;
    checked(ctx, [&] {
        pdf_page_transform(ctx, reinterpret_cast<pdf_page *>(page.get()), &media, &transform);
    });
    return transform;
}
std::unique_ptr<Trace> tracePage(fz_context *ctx, pdf_document *doc, int index) {
    auto trace = std::make_unique<Trace>(ctx);
    auto *page = pageObject(ctx, doc, index);
    State s(ctx, doc, trace.get());
    s.graphics.ctm = pageTransform(ctx, doc, index);
    process(s, inherited(ctx, page, "Resources"), get(ctx, page, "Contents"));
    Page loaded(ctx, checked(ctx, [&] {
                    return reinterpret_cast<fz_page *>(pdf_load_page(ctx, doc, index));
                }));
    fz_stext_options options{};
    options.flags = FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_PRESERVE_LIGATURES;
    Ref<fz_stext_page, fz_drop_stext_page> extracted(
        ctx,
        checked(ctx, [&] { return fz_new_stext_page_from_page(ctx, loaded.get(), &options); }));
    std::vector<Glyph> characters;
    for (auto *block = extracted.get()->first_block; block; block = block->next)
        if (block->type == FZ_STEXT_BLOCK_TEXT)
            for (auto *line = block->u.t.first_line; line; line = line->next)
                for (auto *ch = line->first_char; ch; ch = ch->next)
                    characters.push_back({ch->c, ch->origin});
    for (auto &run : trace->runs) {
        if (run.info.status != TextStatus::OriginalFont)
            continue;
        for (const auto &glyph : run.glyphs) {
            if (std::none_of(characters.begin(), characters.end(), [&](const auto &ch) {
                    return ch.unicode == glyph.unicode &&
                           std::abs(ch.origin.x - glyph.origin.x) < .1 &&
                           std::abs(ch.origin.y - glyph.origin.y) < .1;
                })) {
                run.info.status = TextStatus::Unsupported;
                run.info.reason = "Glyph trace does not match structured-text extraction";
                break;
            }
        }
    }
    return trace;
}
pdf_obj *copyPageResources(fz_context *ctx, pdf_document *doc, int index) {
    return copyResources(ctx, doc, inherited(ctx, pageObject(ctx, doc, index), "Resources"));
}
void appendContent(fz_context *ctx, pdf_document *doc, int index, pdf_obj *resources,
                   fz_buffer *body) {
    auto *obj = pageObject(ctx, doc, index);
    auto *old = get(ctx, obj, "Contents");
    int prepend = 0, append = 0;
    checked(ctx, [&] { pdf_count_q_balance(ctx, doc, resources, old, &prepend, &append); });
    Buffer before(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 64); }));
    Buffer after(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 128); }));
    checked(ctx, [&] {
        for (int i = 0; i <= prepend; ++i)
            fz_append_string(ctx, before.get(), "q\n");
        for (int i = 0; i <= append; ++i)
            fz_append_string(ctx, after.get(), "Q\n");
        fz_append_buffer(ctx, after.get(), body);
    });
    Object array(ctx, checked(ctx, [&] { return pdf_new_array(ctx, doc, 4); }));
    Object start(ctx,
                 checked(ctx, [&] { return pdf_add_stream(ctx, doc, before.get(), nullptr, 0); }));
    Object end(ctx,
               checked(ctx, [&] { return pdf_add_stream(ctx, doc, after.get(), nullptr, 0); }));
    checked(ctx, [&] { pdf_array_push(ctx, array.get(), start.get()); });
    if (checked(ctx, [&] { return pdf_is_array(ctx, old); })) {
        const int n = checked(ctx, [&] { return pdf_array_len(ctx, old); });
        for (int i = 0; i < n; ++i)
            checked(ctx, [&] { pdf_array_push(ctx, array.get(), pdf_array_get(ctx, old, i)); });
    } else if (old)
        checked(ctx, [&] { pdf_array_push(ctx, array.get(), old); });
    checked(ctx, [&] { pdf_array_push(ctx, array.get(), end.get()); });
    put(ctx, obj, "Contents", array.get());
    put(ctx, obj, "Resources", resources);
}
EncodedText encodeReplacement(fz_context *ctx, pdf_document *doc, const TracedRun &original,
                              const TextReplacement &replacement) {
    TracedRun run = original;
    if (replacement.fontSize != 0) {
        if (!std::isfinite(replacement.fontSize) || replacement.fontSize < .0001 ||
            replacement.fontSize > 1000000)
            throw Error(ErrorCode::InvalidArgument, "Font size must be 1–1200 points");
        run.textState.size = replacement.fontSize;
    }
    if (run.info.status == TextStatus::Unsupported)
        throw Error(ErrorCode::Unsupported, run.info.reason);
    if (run.info.status == TextStatus::NeedsFont && replacement.fontFile.empty())
        throw Error(ErrorCode::NeedsFont, run.info.reason);
    std::pair<std::string, double> originalEncoding;
    bool rebindOriginal = false;
    if (replacement.fontFile.empty()) {
        try {
            originalEncoding = encode(ctx, run, replacement.text);
        } catch (const Error &error) {
            if (error.code() != ErrorCode::NeedsFont || !run.font->is_embedded ||
                !checked(ctx, [&] { return pdf_font_writing_supported(ctx, run.font->font); }))
                throw;
            // Re-encode against the same embedded font program, in a private
            // Identity-H resource. Never modify shared Encoding/ToUnicode/W.
            for (int cp : codepoints(replacement.text))
                if (checked(ctx, [&] { return fz_encode_character(ctx, run.font->font, cp); }) <= 0)
                    throw;
            rebindOriginal = true;
        }
    }
    if (replacement.fontIndex < 0)
        throw Error(ErrorCode::InvalidArgument, "Font collection index must be nonnegative");
    Font imported(ctx, replacement.fontFile.empty() ? nullptr : checked(ctx, [&] {
                      return fz_new_font_from_file(ctx, nullptr, replacement.fontFile.c_str(),
                                                   replacement.fontIndex, 0);
                  }));
    auto *candidate = imported.get() ? imported.get() : rebindOriginal ? run.font->font : nullptr;
    unsigned char *program = nullptr;
    const auto programSize =
        candidate && candidate->buffer
            ? checked(ctx, [&] { return fz_buffer_storage(ctx, candidate->buffer, &program); })
            : 0;
    Buffer singleFace(ctx,
                      programSize >= 4 && !std::memcmp(program, "ttcf", 4)
                          ? checked(ctx, [&] { return fz_extract_ttf_from_ttc(ctx, candidate); })
                          : nullptr);
    Font normalized(ctx, singleFace.get() ? checked(ctx,
                                                    [&] {
                                                        return fz_new_font_from_buffer(
                                                            ctx, nullptr, singleFace.get(), 0, 0);
                                                    })
                                          : nullptr);
    auto *selectedFont = normalized.get() ? normalized.get() : candidate;
    Object added(ctx, selectedFont
                          ? checked(ctx, [&] { return pdf_add_cid_font(ctx, doc, selectedFont); })
                          : nullptr);
    Object fontCopy(ctx, rebindOriginal
                             ? checked(ctx, [&] { return pdf_copy_dict(ctx, added.get()); })
                             : nullptr);
    if (rebindOriginal) {
        checked(ctx, [&] {
            pdf_dict_put_name(ctx, fontCopy.get(), PDF_NAME(BaseFont), run.info.fontName.c_str());
        });
    }
    Object importedObject(
        ctx, rebindOriginal ? checked(ctx, [&] { return pdf_add_object(ctx, doc, fontCopy.get()); })
                            : pdf_keep_obj(ctx, added.get()));
    std::string bytes;
    double advance = 0;
    if (selectedFont) {
        for (int cp : codepoints(replacement.text)) {
            if (complexScript(cp))
                throw Error(ErrorCode::Unsupported, "Replacement requires complex shaping");
            const int gid =
                checked(ctx, [&] { return fz_encode_character(ctx, selectedFont, cp); });
            if (gid <= 0 || gid > 65535)
                throw Error(ErrorCode::NeedsFont, "Imported font lacks " + utf8(cp));
            bytes.push_back(char(gid >> 8));
            bytes.push_back(char(gid));
            advance += (checked(ctx, [&] { return fz_advance_glyph(ctx, selectedFont, gid, 0); }) *
                            run.textState.size +
                        run.textState.char_space) *
                       run.textState.scale;
        }
    } else {
        bytes = std::move(originalEncoding.first);
        advance = originalEncoding.second;
    }
    EncodedText result(ctx);
    result.font.reset(
        pdf_keep_obj(ctx, importedObject.get() ? importedObject.get() : run.fontObject));
    result.bytes = std::move(bytes);
    result.advance = advance;
    result.size = run.textState.size;
    result.replacesFont = importedObject.get() != nullptr || replacement.fontSize != 0;
    return result;
}
double writeTextReplacement(fz_context *ctx, pdf_document *doc, int page, const TracedRun &original,
                            const TextReplacement &replacement, const fz_matrix *desiredStart) {
    auto encoded = encodeReplacement(ctx, doc, original, replacement);
    TracedRun run = original;
    run.textState.size = encoded.size;
    const auto &bytes = encoded.bytes;
    const double advance = encoded.advance;
    if (!replacement.allowOverflow && advance > run.advance + .05)
        throw Error(ErrorCode::Overflow, "Replacement is wider than the original run");
    Object array(ctx, checked(ctx, [&] { return pdf_new_array(ctx, doc, 2); }));
    checked(ctx, [&] { pdf_array_push_string(ctx, array.get(), bytes.data(), bytes.size()); });
    const double adjustment =
        (advance - run.advance) * 1000 / (run.textState.size * run.textState.scale);
    checked(ctx, [&] { pdf_array_push_real(ctx, array.get(), float(adjustment)); });
    auto *obj = pageObject(ctx, doc, page);
    Object resources(ctx, copyResources(ctx, doc, inherited(ctx, obj, "Resources")));
    Rewrite e{ctx, doc, &run.forms};
    e.targetShow = run.showIndex;
    Object shiftBefore(ctx), shiftAfter(ctx);
    if (desiredStart) {
        const double square = double(original.start.a) * original.start.a +
                              double(original.start.b) * original.start.b;
        if (square < .000001)
            throw Error(ErrorCode::Unsupported, "Singular text baseline");
        const double x = desiredStart->e - original.start.e, y = desiredStart->f - original.start.f;
        const double shift = (x * original.start.a + y * original.start.b) / square;
        if (std::hypot(x - shift * original.start.a, y - shift * original.start.b) > .05)
            throw Error(ErrorCode::Unsupported, "Segments do not share a baseline");
        if (std::abs(shift) > .000001) {
            // Position-only TJ operators move the text matrix without changing
            // the line matrix, so subsequent Td/T* operators stay untouched.
            shiftBefore.reset(checked(ctx, [&] { return pdf_new_array(ctx, doc, 1); }));
            shiftAfter.reset(checked(ctx, [&] { return pdf_new_array(ctx, doc, 1); }));
            auto *before = shiftBefore.get(), *after = shiftAfter.get();
            e.shiftBefore = before;
            e.shiftAfter = after;
            const double adjustment = -shift * 1000 / (run.textState.size * run.textState.scale);
            checked(ctx, [&] {
                pdf_array_push_real(ctx, before, adjustment);
                pdf_array_push_real(ctx, after, -adjustment);
            });
            // Owned below until the entire recursive stream rewrite finishes.
        }
    }
    e.changeColor = replacement.changeColor;
    e.color = replacement.color;
    for (double v : e.color)
        if (!std::isfinite(v) || v < 0 || v > 1)
            throw Error(ErrorCode::InvalidArgument, "Invalid text colour");
    e.replacementArray = array.get();
    if (encoded.replacesFont) {
        e.replacementFont = encoded.font.get();
        e.replacementFontName = addResource(ctx, resources.get(), "Font", e.replacementFont);
        e.originalFontName = run.fontResource;
        e.fontSize = run.textState.size;
        e.originalFontSize = original.textState.size;
        e.originalFont = run.fontObject;
        if (!get(ctx, get(ctx, resources.get(), "Font"), e.originalFontName.c_str()))
            e.originalFontName = addResource(ctx, resources.get(), "Font", run.fontObject);
    }
    updatePage(ctx, doc, page, e, resources.get());
    return advance;
}
void replaceShowWithForm(fz_context *ctx, pdf_document *doc, int page, const TracedRun &run,
                         pdf_obj *form) {
    if (!run.forms.empty())
        throw Error(ErrorCode::Unsupported,
                    "Wrapping text inside an existing Form is not supported");
    Object resources(ctx, copyPageResources(ctx, doc, page));
    Rewrite e{ctx, doc, &run.forms};
    e.targetShow = run.showIndex;
    e.wrappedForm = form;
    e.wrappedName = addResource(ctx, resources.get(), "XObject", form);
    e.originalFontName = addResource(ctx, resources.get(), "Font", run.fontObject);
    e.originalFontSize = run.textState.size;
    e.originalLineMatrix = run.lineMatrix;
    const double a = run.lineMatrix.a, b = run.lineMatrix.b, square = a * a + b * b;
    if (square < .000001)
        throw Error(ErrorCode::Unsupported, "Singular line matrix");
    const double delta =
        ((run.end.e - run.lineMatrix.e) * a + (run.end.f - run.lineMatrix.f) * b) / square;
    Object after(ctx, checked(ctx, [&] { return pdf_new_array(ctx, doc, 1); }));
    checked(ctx, [&] {
        pdf_array_push_real(ctx, after.get(),
                            -delta * 1000 / (run.textState.size * run.textState.scale));
    });
    e.shiftAfter = after.get();
    updatePage(ctx, doc, page, e, resources.get());
}

void writeImageEdit(fz_context *ctx, pdf_document *doc, int page, const TracedImage &image,
                    const ImageEdit &change) {
    auto *obj = pageObject(ctx, doc, page);
    Object resources(ctx, copyResources(ctx, doc, inherited(ctx, obj, "Resources")));
    Rewrite e{ctx, doc, &image.forms};
    e.targetImage = image.occurrence;
    e.imageAction = change.action;
    if (change.action == ImageAction::Transform) {
        const auto desired = matrix(change.transform);
        for (double v : change.transform)
            if (!std::isfinite(float(v)))
                throw Error(ErrorCode::InvalidArgument,
                            "Image transform must fit finite renderer coordinates");
        if (std::abs(double(desired.a) * desired.d - double(desired.b) * desired.c) < .000001)
            throw Error(ErrorCode::InvalidArgument, "Image transform must be nonsingular");
        if (std::abs(fz_matrix_expansion(image.ctm)) < .0001)
            throw Error(ErrorCode::Unsupported, "Singular image transform");
        e.imageDelta = fz_concat(desired, fz_invert_matrix(image.ctm));
    }
    Image replacement(
        ctx,
        change.action == ImageAction::Replace
            ? checked(ctx,
                      [&] { return fz_new_image_from_file(ctx, change.replacementFile.c_str()); })
            : nullptr);
    Object replacementObject(
        ctx, replacement.get()
                 ? checked(ctx, [&] { return pdf_add_image(ctx, doc, replacement.get()); })
                 : nullptr);
    if (replacementObject.get()) {
        e.replacementImage = replacementObject.get();
        e.replacementName = addResource(ctx, resources.get(), "XObject", replacementObject.get());
    }
    updatePage(ctx, doc, page, e, resources.get());
}
void appendImage(fz_context *ctx, pdf_document *doc, int page, const std::string &path,
                 const Matrix &displayTransform) {
    Image image(ctx, checked(ctx, [&] { return fz_new_image_from_file(ctx, path.c_str()); }));
    Object imageObject(ctx, checked(ctx, [&] { return pdf_add_image(ctx, doc, image.get()); }));
    appendImageObject(ctx, doc, page, imageObject.get(), displayTransform);
}
void appendImageObject(fz_context *ctx, pdf_document *doc, int page, pdf_obj *imageObject,
                       const Matrix &displayTransform) {
    for (double v : displayTransform)
        if (!std::isfinite(float(v)))
            throw Error(ErrorCode::InvalidArgument,
                        "Image transform must fit finite renderer coordinates");
    if (std::abs(displayTransform[0] * displayTransform[3] -
                 displayTransform[1] * displayTransform[2]) < .000001)
        throw Error(ErrorCode::InvalidArgument, "Image transform must be nonsingular");
    const auto m =
        fz_concat(matrix(displayTransform), fz_invert_matrix(pageTransform(ctx, doc, page)));
    auto *obj = pageObject(ctx, doc, page);
    Object resources(ctx, copyResources(ctx, doc, inherited(ctx, obj, "Resources")));
    const auto name = addResource(ctx, resources.get(), "XObject", imageObject);
    // Balance the existing content before appending, including malformed q/Q.
    auto *old = get(ctx, obj, "Contents");
    int prepend = 0, append = 0;
    checked(ctx, [&] { pdf_count_q_balance(ctx, doc, resources.get(), old, &prepend, &append); });
    Buffer before(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 64); }));
    Buffer after(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 128); }));
    checked(ctx, [&] {
        for (int i = 0; i <= prepend; ++i)
            fz_append_string(ctx, before.get(), "q\n");
        for (int i = 0; i <= append; ++i)
            fz_append_string(ctx, after.get(), "Q\n");
        fz_append_printf(ctx, after.get(), "q %g %g %g %g %g %g cm /%s Do Q\n", m.a, m.b, m.c, m.d,
                         m.e, m.f, name.c_str());
    });
    Object a(ctx, checked(ctx, [&] { return pdf_new_array(ctx, doc, 4); }));
    Object start(ctx,
                 checked(ctx, [&] { return pdf_add_stream(ctx, doc, before.get(), nullptr, 0); }));
    Object end(ctx,
               checked(ctx, [&] { return pdf_add_stream(ctx, doc, after.get(), nullptr, 0); }));
    checked(ctx, [&] { pdf_array_push(ctx, a.get(), start.get()); });
    if (checked(ctx, [&] { return pdf_is_array(ctx, old); })) {
        const int n = checked(ctx, [&] { return pdf_array_len(ctx, old); });
        for (int i = 0; i < n; ++i)
            checked(ctx, [&] { pdf_array_push(ctx, a.get(), pdf_array_get(ctx, old, i)); });
    } else if (old)
        checked(ctx, [&] { pdf_array_push(ctx, a.get(), old); });
    checked(ctx, [&] { pdf_array_push(ctx, a.get(), end.get()); });
    put(ctx, obj, "Contents", a.get());
    put(ctx, obj, "Resources", resources.get());
}
RasterImage decodeImage(fz_context *ctx, fz_image *image) {
    if (image->w <= 0 || image->h <= 0 || std::uint64_t(image->w) * image->h > 64 * 1024 * 1024)
        throw Error(ErrorCode::InvalidArgument, "Image exceeds 64 megapixels");
    Ref<fz_pixmap, fz_drop_pixmap> pix(
        ctx, checked(ctx, [&] { return fz_get_unscaled_pixmap_from_image(ctx, image); }));
    Ref<fz_pixmap, fz_drop_pixmap> mask(
        ctx, image->mask
                 ? checked(ctx, [&] { return fz_get_unscaled_pixmap_from_image(ctx, image->mask); })
                 : nullptr);
    Ref<fz_pixmap, fz_drop_pixmap> rgba(ctx, checked(ctx, [&] {
                                            return fz_convert_pixmap(
                                                ctx, pix.get(), fz_device_rgb(ctx), nullptr,
                                                nullptr, fz_default_color_params, 1);
                                        }));
    RasterImage result;
    result.width = rgba.get()->w;
    result.height = rgba.get()->h;
    if (std::uint64_t(result.width) * result.height > 64 * 1024 * 1024)
        throw Error(ErrorCode::InvalidArgument, "Image exceeds 64 megapixels");
    result.rgba.resize(std::size_t(result.width) * result.height * 4);
    const int channels = rgba.get()->n;
    for (int y = 0; y < result.height; ++y)
        for (int x = 0; x < result.width; ++x) {
            const auto *src = rgba.get()->samples + y * rgba.get()->stride + x * channels;
            auto *dst = &result.rgba[(std::size_t(y) * result.width + x) * 4];
            const int alpha = channels == 4 ? src[3] : 255;
            for (int c = 0; c < 3; ++c)
                dst[c] = alpha ? std::min(255, int(src[c]) * 255 / alpha) : 0; // Unpremultiply.
            dst[3] =
                mask.get()
                    ? mask.get()->samples[(y * mask.get()->h / result.height) * mask.get()->stride +
                                          (x * mask.get()->w / result.width) * mask.get()->n]
                    : alpha;
        }
    return result;
}
std::string extractText(fz_context *ctx, pdf_document *doc, int index) {
    Page page(ctx, checked(ctx, [&] {
                  return reinterpret_cast<fz_page *>(pdf_load_page(ctx, doc, index));
              }));
    fz_stext_options options{};
    options.flags = FZ_STEXT_PRESERVE_WHITESPACE;
    Ref<fz_stext_page, fz_drop_stext_page> text(
        ctx, checked(ctx, [&] { return fz_new_stext_page_from_page(ctx, page.get(), &options); }));
    std::string result;
    for (auto *b = text.get()->first_block; b; b = b->next)
        if (b->type == FZ_STEXT_BLOCK_TEXT)
            for (auto *line = b->u.t.first_line; line; line = line->next) {
                for (auto *c = line->first_char; c; c = c->next)
                    result += utf8(c->c);
                result += '\n';
            }
    return result;
}
std::string rewriteAppearance(fz_context *ctx, pdf_document *doc, pdf_obj *resources,
                              pdf_obj *appearance,
                              const std::map<std::string, std::string> &names) {
    struct Mapper {
        pdf_processor super;
        const std::map<std::string, std::string> *names;
    };
    const auto *bytes = checked(ctx, [&] { return pdf_to_str_buf(ctx, appearance); });
    const auto length = checked(ctx, [&] { return pdf_to_str_len(ctx, appearance); });
    Buffer input(ctx, checked(ctx, [&] {
                     return fz_new_buffer_from_copied_data(
                         ctx, reinterpret_cast<const unsigned char *>(bytes), length);
                 }));
    Buffer output(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 64); }));
    Processor chain(
        ctx, checked(ctx, [&] { return pdf_new_buffer_processor(ctx, output.get(), 1, 1); }));
    Processor proc(ctx, checked(ctx, [&] {
                       return static_cast<pdf_processor *>(pdf_new_processor(ctx, sizeof(Mapper)));
                   }));
    auto *mapper = reinterpret_cast<Mapper *>(proc.get());
    mapper->names = &names;
    mapper->super.chain = chain.get();
    mapper->super.drop_processor = nullptr;
    mapper->super.op_Tf = [](fz_context *c, pdf_processor *p, const char *name, pdf_font_desc *font,
                             float size) {
        callback(c, [&] {
            const auto &names = *reinterpret_cast<Mapper *>(p)->names;
            auto found = names.find(name);
            checked(c, [&] {
                p->chain->op_Tf(c, p->chain, found == names.end() ? name : found->second.c_str(),
                                font, size);
            });
        });
    };
    mapper->super.op_cs = [](fz_context *c, pdf_processor *p, const char *name, fz_colorspace *cs) {
        callback(c, [&] {
            const auto &names = *reinterpret_cast<Mapper *>(p)->names;
            auto found = names.find(name);
            checked(c, [&] {
                p->chain->op_cs(c, p->chain, found == names.end() ? name : found->second.c_str(),
                                cs);
            });
        });
    };
    mapper->super.op_CS = [](fz_context *c, pdf_processor *p, const char *name, fz_colorspace *cs) {
        callback(c, [&] {
            const auto &names = *reinterpret_cast<Mapper *>(p)->names;
            auto found = names.find(name);
            checked(c, [&] {
                p->chain->op_CS(c, p->chain, found == names.end() ? name : found->second.c_str(),
                                cs);
            });
        });
    };
    mapper->super.op_gs_begin = [](fz_context *c, pdf_processor *p, const char *name,
                                   pdf_obj *object) {
        callback(c, [&] {
            const auto &names = *reinterpret_cast<Mapper *>(p)->names;
            auto found = names.find(name);
            checked(c, [&] {
                p->chain->op_gs_begin(c, p->chain,
                                      found == names.end() ? name : found->second.c_str(), object);
            });
        });
    };
    checked(ctx, [&] {
        pdf_process_glyph(ctx, proc.get(), doc, resources, input.get());
        pdf_close_processor(ctx, proc.get());
    });
    unsigned char *result;
    const auto size = checked(ctx, [&] { return fz_buffer_storage(ctx, output.get(), &result); });
    return {reinterpret_cast<char *>(result), size};
}
void appendText(fz_context *ctx, pdf_document *doc, int index, const AddTextSpec &spec) {
    pageObject(ctx, doc, index);
    if (spec.fontFile.empty() || spec.text.empty() || spec.text.size() > 1024 * 1024 ||
        spec.fontIndex < 0)
        throw Error(ErrorCode::InvalidArgument, "Choose a font and nonempty text");
    for (double v : {spec.x, spec.y, spec.width, spec.height, spec.fontSize})
        if (!std::isfinite(v) || std::abs(v) > 1000000)
            throw Error(ErrorCode::InvalidArgument, "Invalid text geometry");
    if (spec.fontSize < 1 || spec.fontSize > 1200 || spec.width < 0 || spec.height < 0 ||
        spec.alignment < 0 || spec.alignment > 2)
        throw Error(ErrorCode::InvalidArgument, "Invalid size or alignment");
    for (double v : spec.color)
        if (!std::isfinite(v) || v < 0 || v > 1)
            throw Error(ErrorCode::InvalidArgument, "Invalid colour");
    Font font(ctx, checked(ctx, [&] {
                  return fz_new_font_from_file(ctx, nullptr, spec.fontFile.c_str(), spec.fontIndex,
                                               0);
              }));
    unsigned char *program;
    const auto size =
        checked(ctx, [&] { return fz_buffer_storage(ctx, font.get()->buffer, &program); });
    Buffer single(ctx, size >= 4 && !std::memcmp(program, "ttcf", 4)
                           ? checked(ctx, [&] { return fz_extract_ttf_from_ttc(ctx, font.get()); })
                           : nullptr);
    Font normalized(ctx, single.get() ? checked(ctx,
                                                [&] {
                                                    return fz_new_font_from_buffer(
                                                        ctx, nullptr, single.get(), 0, 0);
                                                })
                                      : nullptr);
    auto *selected = normalized.get() ? normalized.get() : font.get();
    Object resource(ctx, checked(ctx, [&] { return pdf_add_cid_font(ctx, doc, selected); }));
    Object resources(ctx, copyPageResources(ctx, doc, index));
    const auto name = addResource(ctx, resources.get(), "Font", resource.get());
    struct Line {
        std::vector<int> glyphs;
        double width{};
    };
    std::vector<Line> lines(1);
    std::size_t start = 0;
    while (start <= spec.text.size()) {
        auto end = spec.text.find('\n', start);
        if (end == std::string::npos)
            end = spec.text.size();
        for (int cp : codepoints(spec.text.substr(start, end - start))) {
            if (complexScript(cp))
                throw Error(ErrorCode::Unsupported, "This script requires shaping");
            const int gid = checked(ctx, [&] { return fz_encode_character(ctx, selected, cp); });
            if (gid <= 0 || gid > 65535)
                throw Error(ErrorCode::NeedsFont, "Font lacks " + utf8(cp));
            const double advance =
                checked(ctx, [&] { return fz_advance_glyph(ctx, selected, gid, 0); }) *
                spec.fontSize;
            if (spec.width > 0 && advance > spec.width)
                throw Error(ErrorCode::Overflow, "Text box is narrower than a glyph");
            if (spec.width > 0 && !lines.back().glyphs.empty() &&
                lines.back().width + advance > spec.width)
                lines.emplace_back();
            lines.back().glyphs.push_back(gid);
            lines.back().width += advance;
        }
        if (end == spec.text.size())
            break;
        lines.emplace_back();
        start = end + 1;
    }
    if (spec.height > 0 && lines.size() * spec.fontSize * 1.2 > spec.height)
        throw Error(ErrorCode::Overflow, "Text exceeds the box height");
    Buffer contents(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 1024); }));
    const auto inverse = fz_invert_matrix(pageTransform(ctx, doc, index));
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const double offset = spec.width > 0
                                  ? (spec.width - lines[i].width) * (spec.alignment == 1   ? .5
                                                                     : spec.alignment == 2 ? 1
                                                                                           : 0)
                                  : 0;
        const auto m = fz_concat(
            {1, 0, 0, -1, float(spec.x + offset), float(spec.y + i * spec.fontSize * 1.2)},
            inverse);
        checked(ctx, [&] {
            fz_append_printf(ctx, contents.get(),
                             "q BT /%s %g Tf %g %g %g rg %g %g %g %g %g %g Tm <", name.c_str(),
                             spec.fontSize, spec.color[0], spec.color[1], spec.color[2], m.a, m.b,
                             m.c, m.d, m.e, m.f);
        });
        for (int gid : lines[i].glyphs)
            checked(ctx, [&] { fz_append_printf(ctx, contents.get(), "%04x", gid); });
        checked(ctx, [&] { fz_append_string(ctx, contents.get(), "> Tj ET Q\n"); });
    }
    appendContent(ctx, doc, index, resources.get(), contents.get());
}
} // namespace yolo::engine::mupdf
