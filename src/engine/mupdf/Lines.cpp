// SPDX-License-Identifier: AGPL-3.0-only
#include "Lines.h"
#include <algorithm>
#include <cmath>
namespace yolo::engine::mupdf {
namespace {
fz_matrix display(const TracedRun &r) {
    return fz_concat(r.start, matrix(r.info.ctm));
}
double scaleX(const TracedRun &r) {
    auto m = display(r);
    return std::hypot(m.a, m.b);
}
double scaleY(const TracedRun &r) {
    auto m = display(r);
    return std::hypot(m.c, m.d);
}
bool adjacent(const TracedRun &a, const TracedRun &b) {
    const auto x = display(a), y = display(b);
    const double ax = std::hypot(x.a, x.b), bx = std::hypot(y.a, y.b);
    if (ax < .0001 || bx < .0001)
        return false;
    const double dx = x.a / ax, dy = x.b / ax;
    if (std::abs(dx - y.a / bx) > .001 || std::abs(dy - y.b / bx) > .001)
        return false;
    const double along = (y.e - x.e) * dx + (y.f - x.f) * dy,
                 across = -(y.e - x.e) * dy + (y.f - x.f) * dx;
    const double tolerance =
        .75 * std::max(a.textState.size * scaleY(a), b.textState.size * scaleY(b));
    return std::abs(across) <= .1 && along >= -.05 && along <= a.advance * ax + tolerance;
}
} // namespace
std::vector<EditableLine> buildLines(const Trace &trace) {
    std::vector<EditableLine> lines;
    const TracedRun *previous = nullptr;
    for (const auto &run : trace.runs) {
        if (run.info.text.empty())
            continue;
        if (lines.empty() || !previous || !adjacent(*previous, run)) {
            EditableLine line;
            line.id = run.info.id;
            line.bounds = run.info.bounds;
            lines.push_back(std::move(line));
        }
        auto &line = lines.back();
        int offset = 0;
        for (auto &s : line.segments)
            offset += s.length;
        line.segments.push_back({run.info.id, offset, int(run.glyphs.size()), run.info});
        line.text += run.info.text;
        line.bounds = rect(fz_union_rect(rect(line.bounds), rect(run.info.bounds)));
        if (run.info.status == TextStatus::Unsupported) {
            line.status = TextStatus::Unsupported;
            line.reason = run.info.reason;
        } else if (run.info.status == TextStatus::NeedsFont &&
                   line.status != TextStatus::Unsupported) {
            line.status = TextStatus::NeedsFont;
            line.reason = run.info.reason;
        }
        if (offset + run.glyphs.size() > 2048) {
            line.status = TextStatus::Unsupported;
            line.reason = "Lines longer than 2048 characters are not editable";
        }
        previous = &run;
    }
    return lines;
}
std::vector<ExpectedSegment> writeLine(fz_context *ctx, pdf_document *doc, int page,
                                       const Trace &trace, const EditableLine &line,
                                       const TextReplacement &edit) {
    if (line.status == TextStatus::Unsupported)
        throw Error(ErrorCode::Unsupported, line.reason);
    const auto original = editCharacters(line.text), next = editCharacters(edit.text);
    if (original.size() > 2048 || next.size() > 2048)
        throw Error(ErrorCode::InvalidArgument, "Editable lines are limited to 2048 characters");
    std::vector<int> owners;
    for (int i = 0; i < int(line.segments.size()); ++i)
        owners.insert(owners.end(), line.segments[i].length, i);
    if (owners.size() != original.size() || owners.empty())
        throw Error(ErrorCode::Unsupported, "Text/style mapping is ambiguous");
    // A bounded LCS preserves the style of retained characters, including edits
    // at several positions. Insertions inherit the preceding retained style.
    const auto columns = next.size() + 1;
    std::vector<std::uint16_t> lcs((original.size() + 1) * columns);
    for (int i = int(original.size()) - 1; i >= 0; --i)
        for (int j = int(next.size()) - 1; j >= 0; --j)
            lcs[i * columns + j] = original[i] == next[j] ? 1 + lcs[(i + 1) * columns + j + 1]
                                                          : std::max(lcs[(i + 1) * columns + j],
                                                                     lcs[i * columns + j + 1]);
    std::vector<std::vector<int>> parts(line.segments.size());
    std::size_t i = 0, j = 0;
    int preceding = owners[0];
    while (j < next.size()) {
        if (i < original.size() && original[i] == next[j]) {
            preceding = owners[i];
            parts[preceding].push_back(next[j]);
            ++i;
            ++j;
        } else if (i < original.size() && lcs[(i + 1) * columns + j] >= lcs[i * columns + j + 1])
            ++i;
        else {
            parts[preceding].push_back(next[j]);
            ++j;
        }
    }
    const auto &first = trace.runs[line.segments.front().runId];
    const auto &last = trace.runs[line.segments.back().runId];
    const auto firstMatrix = display(first);
    const double length = std::hypot(firstMatrix.a, firstMatrix.b);
    if (length < .0001)
        throw Error(ErrorCode::Unsupported, "Singular text baseline");
    const double dx = firstMatrix.a / length, dy = firstMatrix.b / length;
    const auto lastEnd = fz_transform_point({last.end.e, last.end.f}, matrix(last.info.ctm));
    const double capacity = (lastEnd.x - firstMatrix.e) * dx + (lastEnd.y - firstMatrix.f) * dy;
    double advance = 0;
    std::vector<ExpectedSegment> expected;
    for (std::size_t n = 0; n < line.segments.size(); ++n) {
        const auto &run = trace.runs[line.segments[n].runId];
        auto desired = run.start;
        const fz_point position{float(firstMatrix.e + dx * advance),
                                float(firstMatrix.f + dy * advance)};
        const auto local = fz_transform_point(position, fz_invert_matrix(matrix(run.info.ctm)));
        desired.e = local.x;
        desired.f = local.y;
        auto part = edit;
        part.runId = run.info.id;
        part.text = editString(parts[n]);
        part.allowOverflow = true;
        if (edit.fontSize != 0) {
            const double sy = scaleY(run);
            if (sy < .0001)
                throw Error(ErrorCode::Unsupported, "Singular text size");
            part.fontSize = edit.fontSize / sy;
        }
        const double width = writeTextReplacement(ctx, doc, page, run, part, &desired);
        advance += width * scaleX(run);
        expected.push_back({run.info.id, part.text,
                            edit.fontFile.empty() ? run.info.fontName : std::string(), position});
    }
    if (!edit.allowOverflow && advance > capacity + .1)
        throw Error(ErrorCode::Overflow, "Replacement is wider than the original line; widen "
                                         "explicitly or choose a smaller size");
    return expected;
}
} // namespace yolo::engine::mupdf
