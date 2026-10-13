// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Content.h"
namespace yolo::engine::mupdf {
std::vector<EditableLine> buildLines(const Trace &);
struct ExpectedSegment {
    int runId;
    std::string text, font;
    fz_point baseline;
};
std::vector<ExpectedSegment> writeLine(fz_context *, pdf_document *, int, const Trace &,
                                       const EditableLine &, const TextReplacement &);
std::vector<ExpectedSegment> writeWrappedLine(fz_context *, pdf_document *, int, const Trace &,
                                              const EditableLine &, const TextReplacement &);
} // namespace yolo::engine::mupdf
