// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Support.h"
namespace yolo::engine::mupdf {
void addStamp(fz_context *, pdf_document *, int, const StampSpec &);
void insertPages(fz_context *, pdf_document *, int, const std::vector<PageSource> &);
void blankPage(fz_context *, pdf_document *, int, double, double);
void rearrange(fz_context *, pdf_document *, const std::vector<int> &);
std::vector<std::uint8_t> serialize(fz_context *, pdf_document *);
void flattenStamps(fz_context *, pdf_document *);
} // namespace yolo::engine::mupdf
