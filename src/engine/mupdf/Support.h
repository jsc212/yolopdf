// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "engine/api/Document.h"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <type_traits>

namespace yolo::engine::mupdf {
// Callbacks contain C operations and trivial locals only. Keep C++ RAII outside
// fz_try because MuPDF's exceptions use longjmp.
template <class F> auto checked(fz_context *ctx, F function) -> decltype(function()) {
    using T = decltype(function());
    if constexpr (std::is_void_v<T>) {
        fz_try(ctx) {
            function();
        }
        fz_catch(ctx) {
            throw Error(ErrorCode::InvalidDocument, fz_caught_message(ctx));
        }
    } else {
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
}
template <class T, void (*Drop)(fz_context *, T *)> class Ref {
  public:
    Ref(fz_context *ctx, T *value = nullptr) : ctx_(ctx), value_(value) {}
    ~Ref() { Drop(ctx_, value_); }
    Ref(const Ref &) = delete;
    Ref &operator=(const Ref &) = delete;
    Ref(Ref &&other) noexcept : ctx_(other.ctx_), value_(other.release()) {}
    T *get() const { return value_; }
    T *release() {
        T *result = value_;
        value_ = nullptr;
        return result;
    }
    void reset(T *value) {
        Drop(ctx_, value_);
        value_ = value;
    }

  private:
    fz_context *ctx_;
    T *value_;
};
using Object = Ref<pdf_obj, pdf_drop_obj>;
using Buffer = Ref<fz_buffer, fz_drop_buffer>;
using Font = Ref<fz_font, fz_drop_font>;
using Image = Ref<fz_image, fz_drop_image>;
using Processor = Ref<pdf_processor, pdf_drop_processor>;
using Page = Ref<fz_page, fz_drop_page>;
inline pdf_obj *get(fz_context *ctx, pdf_obj *obj, const char *key) {
    return checked(ctx, [&] { return pdf_dict_gets(ctx, obj, key); });
}
inline pdf_obj *inherited(fz_context *ctx, pdf_obj *obj, const char *key) {
    return checked(ctx, [&] { return pdf_dict_gets_inheritable(ctx, obj, key); });
}
inline void put(fz_context *ctx, pdf_obj *obj, const char *key, pdf_obj *value) {
    checked(ctx, [&] { pdf_dict_puts(ctx, obj, key, value); });
}
inline Matrix matrix(fz_matrix m) {
    return {m.a, m.b, m.c, m.d, m.e, m.f};
}
inline fz_matrix matrix(const Matrix &m) {
    return {float(m[0]), float(m[1]), float(m[2]), float(m[3]), float(m[4]), float(m[5])};
}
inline Rect rect(fz_rect r) {
    return {r.x0, r.y0, r.x1, r.y1};
}
inline fz_rect rect(Rect r) {
    return {float(r.x0), float(r.y0), float(r.x1), float(r.y1)};
}
inline pdf_obj *pageObject(fz_context *ctx, pdf_document *doc, int index) {
    const int count = checked(ctx, [&] { return pdf_count_pages(ctx, doc); });
    if (index < 0 || index >= count)
        throw Error(ErrorCode::InvalidPage, "Page index out of range");
    return checked(ctx, [&] { return pdf_lookup_page_obj(ctx, doc, index); });
}
fz_matrix pageTransform(fz_context *, pdf_document *, int);
void ensureEditable(fz_context *, pdf_document *, bool allowSigned = false);
bool isSigned(fz_context *, pdf_document *);
void readJournal(fz_context *, pdf_document *, fz_stream *);
// MuPDF 1.28.5's snapshot writer clears num_incremental_sections even in
// snapshot mode. Restore it so subsequent saves and the journal fingerprint
// still refer to the live unsaved changes. All locals here are trivial.
inline void writeSnapshot(fz_context *ctx, pdf_document *doc, fz_output *out) {
    const int incremental = doc->num_incremental_sections;
    fz_try(ctx) {
        pdf_write_snapshot(ctx, doc, out);
    }
    fz_always(ctx) {
        doc->num_incremental_sections = incremental;
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}
inline void invalidateCaches(fz_context *ctx, pdf_document *doc) {
    pdf_drop_resource_tables(ctx, doc);
    doc->resources = {};
    pdf_empty_store(ctx, doc);
    pdf_drop_page_tree_internal(ctx, doc);
    pdf_sync_open_pages(ctx, doc);
}
class Transaction {
  public:
    Transaction(fz_context *ctx, pdf_document *doc, const char *label, bool allowSigned = false)
        : ctx_(ctx), doc_(doc) {
        ensureEditable(ctx, doc, allowSigned);
        checked(ctx, [&] { pdf_begin_operation(ctx, doc, label); });
    }
    ~Transaction() {
        if (!ended_) {
            fz_try(ctx_) {
                pdf_abandon_operation(ctx_, doc_);
                invalidateCaches(ctx_, doc_);
            }
            fz_catch(ctx_) {
                fz_report_error(ctx_);
            }
        }
    }
    void commit() {
        checked(ctx_, [&] { pdf_end_operation(ctx_, doc_); });
        ended_ = true;
    }

  private:
    fz_context *ctx_;
    pdf_document *doc_;
    bool ended_{};
};
} // namespace yolo::engine::mupdf
