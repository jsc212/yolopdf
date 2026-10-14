// SPDX-License-Identifier: AGPL-3.0-only
#include "Support.h"
#include <cstring>

// Interoperability layouts for the pinned MuPDF 1.28.5 journal ABI, whose
// stock deserializer appends fragments to a pending entry it never creates.
// These data layouts must be reviewed whenever BundleMuPDF.cmake is upgraded.
struct JournalFragment {
    JournalFragment *next, *prev;
    int objectNumber, newObject;
    pdf_obj *inactive;
    fz_buffer *stream;
};
struct JournalEntry {
    JournalEntry *prev, *next;
    char *title;
    JournalFragment *head, *tail;
};
struct pdf_journal {
    JournalEntry *head, *current;
    int nesting;
    JournalEntry *pending, *pendingTail;
};
namespace yolo::engine::mupdf {
void readJournal(fz_context *ctx, pdf_document *doc, fz_stream *in) {
    if (doc->journal)
        throw Error(ErrorCode::InvalidArgument, "Recovery requires a fresh document");
    if (checked(ctx, [&] { return fz_skip_string(ctx, in, "%!MuPDF-Journal-100"); }))
        throw Error(ErrorCode::InvalidDocument, "Unsupported journal format");
    checked(ctx, [&] { fz_skip_space(ctx, in); });
    if (checked(ctx, [&] { return fz_skip_string(ctx, in, "journal"); }) ||
        checked(ctx, [&] { return pdf_lex(ctx, in, &doc->lexbuf.base); }) != PDF_TOK_OPEN_DICT)
        throw Error(ErrorCode::InvalidDocument, "Invalid journal header");
    Object header(ctx,
                  checked(ctx, [&] { return pdf_parse_dict(ctx, doc, in, &doc->lexbuf.base); }));
    const int sections =
        checked(ctx, [&] { return pdf_dict_get_int(ctx, header.get(), PDF_NAME(NumSections)); });
    const int position =
        checked(ctx, [&] { return pdf_dict_get_int(ctx, header.get(), PDF_NAME(HistoryPos)); });
    const auto fileSize =
        checked(ctx, [&] { return pdf_dict_get_int64(ctx, header.get(), PDF_NAME(FileSize)); });
    if (sections < 0 || sections > doc->num_xref_sections || position < 0 || fileSize < 0 ||
        fileSize > doc->file_size)
        throw Error(ErrorCode::UnsafeEdit, "Journal does not match the snapshot");
    fz_md5 md5;
    unsigned char actual[16];
    fz_md5_init(&md5);
    fz_md5_update_int64(&md5, doc->num_xref_sections - sections);
    for (int i = sections; i < doc->num_xref_sections; ++i) {
        fz_md5_update_int64(&md5, doc->xref_sections[i].num_objects);
        for (auto *s = doc->xref_sections[i].subsec; s; s = s->next) {
            fz_md5_update_int64(&md5, s->start);
            fz_md5_update_int64(&md5, s->len);
        }
    }
    fz_md5_final(&md5, actual);
    auto *digest = get(ctx, header.get(), "Fingerprint");
    if (checked(ctx, [&] { return pdf_to_str_len(ctx, digest); }) != 16 ||
        std::memcmp(checked(ctx, [&] { return pdf_to_str_buf(ctx, digest); }), actual, 16))
        throw Error(ErrorCode::UnsafeEdit, "Journal fingerprint does not match the snapshot");
    Ref<pdf_journal, pdf_drop_journal> journal(ctx, checked(ctx, [&] {
                                                   return static_cast<pdf_journal *>(
                                                       fz_calloc(ctx, 1, sizeof(pdf_journal)));
                                               }));
    JournalEntry *tail = nullptr;
    int entries = 0, fragments = 0;
    while (true) {
        checked(ctx, [&] { fz_skip_space(ctx, in); });
        if (checked(ctx, [&] { return fz_peek_byte(ctx, in); }) == 'e') {
            char marker[32]{};
            checked(ctx, [&] { fz_read_line(ctx, in, marker, sizeof(marker)); });
            if (!std::strcmp(marker, "endjournal"))
                break;
            if (std::strcmp(marker, "entry"))
                throw Error(ErrorCode::InvalidDocument, "Invalid journal marker");
            if (++entries > 100000 ||
                checked(ctx, [&] { return pdf_lex(ctx, in, &doc->lexbuf.base); }) !=
                    PDF_TOK_STRING ||
                doc->lexbuf.base.len > 65536)
                throw Error(ErrorCode::InvalidDocument, "Invalid journal entry");
            auto *entry = checked(ctx, [&] {
                return static_cast<JournalEntry *>(fz_calloc(ctx, 1, sizeof(JournalEntry)));
            });
            entry->prev = tail;
            if (tail)
                tail->next = entry;
            else
                journal.get()->head = entry;
            tail = entry;
            entry->title = checked(
                ctx, [&] { return static_cast<char *>(fz_malloc(ctx, doc->lexbuf.base.len + 1)); });
            std::memcpy(entry->title, doc->lexbuf.base.buffer, doc->lexbuf.base.len);
            entry->title[doc->lexbuf.base.len] = 0;
            continue;
        }
        if (!tail || ++fragments > 1000000)
            throw Error(ErrorCode::InvalidDocument, "Invalid journal fragments");
        struct Parsed {
            int number{}, fresh{};
            pdf_obj *object{};
            fz_buffer *stream{};
        } parsed;
        parsed.object = checked(ctx, [&] {
            return pdf_parse_journal_obj(ctx, doc, in, &parsed.number, &parsed.stream,
                                         &parsed.fresh);
        });
        Object object(ctx, parsed.object);
        Buffer stream(ctx, parsed.stream);
        if (parsed.number < 0 ||
            parsed.number >= checked(ctx, [&] { return pdf_xref_len(ctx, doc); }))
            throw Error(ErrorCode::InvalidDocument, "Journal object is out of range");
        auto *fragment = checked(ctx, [&] {
            return static_cast<JournalFragment *>(fz_calloc(ctx, 1, sizeof(JournalFragment)));
        });
        fragment->objectNumber = parsed.number;
        fragment->newObject = parsed.fresh;
        fragment->inactive = pdf_keep_obj(ctx, object.get());
        fragment->stream = fz_keep_buffer(ctx, stream.get());
        fragment->prev = tail->tail;
        if (tail->tail)
            tail->tail->next = fragment;
        else
            tail->head = fragment;
        tail->tail = fragment;
    }
    if (position > entries)
        throw Error(ErrorCode::InvalidDocument, "Invalid recovery history position");
    auto *current = journal.get()->head;
    for (int i = 1; i < position; ++i)
        current = current->next;
    journal.get()->current = position ? current : nullptr;
    // Stock journal restoration reclassifies these xrefs as unsaved. Keep the
    // parsed original prefix and direct trailer, with a solid incremental xref.
    checked(ctx, [&] { pdf_ensure_solid_xref(ctx, doc, pdf_xref_len(ctx, doc)); });
    doc->file_size = fileSize;
    doc->num_incremental_sections = sections;
    if (sections > 0) {
        // The snapshot's current startxref points into its appended changes.
        // Repeated snapshots must reference the original prefix instead.
        doc->startxref = checked(ctx, [&] {
            return pdf_dict_get_int64(ctx, doc->xref_sections[sections - 1].trailer,
                                      PDF_NAME(Prev));
        });
        const int trailerObject = pdf_obj_parent_num(ctx, doc->xref_sections[0].trailer);
        checked(ctx, [&] {
            if (trailerObject > 0)
                pdf_delete_object(ctx, doc, trailerObject);
            pdf_set_obj_parent(ctx, doc->xref_sections[0].trailer, 0);
        });
    }
    // Transfer ownership only after parsing/validation has succeeded.
    doc->journal = journal.get();
    // Ref deliberately keeps ownership until this transfer is disarmed.
    journal.release();
}
} // namespace yolo::engine::mupdf
