// SPDX-License-Identifier: AGPL-3.0-only
#include "Render.h"
#include <algorithm>
#include <cmath>
namespace yolo::engine::mupdf {
namespace {
struct Scene {
    fz_context *context{};
    fz_display_list *list{};
    std::shared_ptr<ContextLocks> locks;
    ~Scene() {
        fz_drop_display_list(context, list);
        fz_drop_context(context);
    }
};
class RenderJob final : public IRenderJob {
    std::shared_ptr<Scene> scene_;
    fz_context *ctx_{};

  public:
    explicit RenderJob(std::shared_ptr<Scene> scene) : scene_(std::move(scene)) {
        ctx_ = checked(scene_->context, [&] { return fz_clone_context(scene_->context); });
        if (!ctx_)
            throw Error(ErrorCode::CannotOpen, "Cannot clone rendering context");
    }
    ~RenderJob() { fz_drop_context(ctx_); }
    RasterImage render(double dpi, const Rect &tile) override {
        if (!std::isfinite(dpi) || dpi < 1 || dpi > 1200)
            throw Error(ErrorCode::InvalidArgument, "Invalid rendering DPI");
        auto page = checked(ctx_, [&] { return fz_bound_display_list(ctx_, scene_->list); });
        const auto clip =
            tile.x1 > tile.x0 && tile.y1 > tile.y0 ? fz_intersect_rect(page, rect(tile)) : page;
        const auto transform = fz_scale(dpi / 72, dpi / 72);
        const auto scaled = fz_transform_rect(clip, transform);
        const double w = std::ceil(scaled.x1) - std::floor(scaled.x0),
                     h = std::ceil(scaled.y1) - std::floor(scaled.y0);
        if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w * h > 64 * 1024 * 1024)
            throw Error(ErrorCode::InvalidArgument, "Render exceeds the 64 megapixel limit");
        const auto pixels = fz_round_rect(scaled);
        Ref<fz_pixmap, fz_drop_pixmap> pix(ctx_, checked(ctx_, [&] {
                                               return fz_new_pixmap_with_bbox(
                                                   ctx_, fz_device_rgb(ctx_), pixels, nullptr, 0);
                                           }));
        fz_clear_pixmap_with_value(ctx_, pix.get(), 255);
        Ref<fz_device, fz_drop_device> device(
            ctx_, checked(ctx_, [&] { return fz_new_draw_device(ctx_, fz_identity, pix.get()); }));
        checked(ctx_, [&] {
            fz_run_display_list(ctx_, scene_->list, device.get(), transform,
                                fz_rect_from_irect(pixels), nullptr);
            fz_close_device(ctx_, device.get());
        });
        RasterImage result;
        result.width = pix.get()->w;
        result.height = pix.get()->h;
        result.rgba.resize(std::size_t(result.width) * result.height * 4);
        for (int y = 0; y < result.height; ++y)
            for (int x = 0; x < result.width; ++x) {
                auto *dst = &result.rgba[(std::size_t(y) * result.width + x) * 4];
                auto *src = pix.get()->samples + y * pix.get()->stride + x * 3;
                std::copy_n(src, 3, dst);
                dst[3] = 255;
            }
        return result;
    }
};
class RenderSource final : public IRenderSource {
    std::shared_ptr<Scene> scene_;

  public:
    explicit RenderSource(std::shared_ptr<Scene> scene) : scene_(std::move(scene)) {}
    std::unique_ptr<IRenderJob> job() override { return std::make_unique<RenderJob>(scene_); }
};
} // namespace
std::shared_ptr<IRenderSource> makeRenderSource(fz_context *ctx, pdf_document *doc, int index,
                                                std::shared_ptr<ContextLocks> locks) {
    pageObject(ctx, doc, index);
    Page page(ctx, checked(ctx, [&] {
                  return reinterpret_cast<fz_page *>(pdf_load_page(ctx, doc, index));
              }));
    auto scene = std::make_shared<Scene>();
    scene->locks = std::move(locks);
    scene->context = checked(ctx, [&] { return fz_clone_context(ctx); });
    if (!scene->context)
        throw Error(ErrorCode::CannotOpen, "Cannot allocate rendering scene");
    scene->list = checked(ctx, [&] { return fz_new_display_list_from_page(ctx, page.get()); });
    return std::make_shared<RenderSource>(std::move(scene));
}
} // namespace yolo::engine::mupdf
