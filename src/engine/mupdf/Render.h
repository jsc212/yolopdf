// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "Support.h"
#include <mutex>
namespace yolo::engine::mupdf {
struct ContextLocks {
    std::array<std::mutex, FZ_LOCK_MAX> mutexes;
    fz_locks_context callbacks{
        this, [](void *p, int n) { static_cast<ContextLocks *>(p)->mutexes[n].lock(); },
        [](void *p, int n) { static_cast<ContextLocks *>(p)->mutexes[n].unlock(); }};
};
std::shared_ptr<IRenderSource> makeRenderSource(fz_context *, pdf_document *, int,
                                                std::shared_ptr<ContextLocks>);
} // namespace yolo::engine::mupdf
