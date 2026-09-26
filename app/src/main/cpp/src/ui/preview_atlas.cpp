/**
 * @file preview_atlas.cpp
 * @brief Атлас превью миров: несколько снимков — одна текстура.
 */
#include "preview_atlas.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ui {

namespace {

/// Уменьшение усреднением: каждый пиксель результата — среднее всех
/// исходных, которые на него пришлись. Ближайший сосед на
/// изометрии рвёт рёбра блоков в лесенку, и превью мельчает в шум.
void boxResize(const u8* src, u32 sw, u32 sh,
               u8* dst, u32 dstStride, u32 dw, u32 dh)
{
    for (u32 y = 0; y < dh; ++y) {
        const u32 sy0 = (u32)((u64)y * sh / dh);
        u32 sy1 = (u32)((u64)(y + 1) * sh / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (u32 x = 0; x < dw; ++x) {
            const u32 sx0 = (u32)((u64)x * sw / dw);
            u32 sx1 = (u32)((u64)(x + 1) * sw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            u32 r = 0, g = 0, b = 0, n = 0;
            for (u32 yy = sy0; yy < sy1 && yy < sh; ++yy) {
                const u8* row = src + (usize)yy * sw * 3;
                for (u32 xx = sx0; xx < sx1 && xx < sw; ++xx) {
                    r += row[xx * 3 + 0];
                    g += row[xx * 3 + 1];
                    b += row[xx * 3 + 2];
                    ++n;
                }
            }
            if (n == 0) n = 1;
            u8* o = dst + (usize)y * dstStride + (usize)x * 4;
            o[0] = (u8)(r / n);
            o[1] = (u8)(g / n);
            o[2] = (u8)(b / n);
            o[3] = 255;
        }
    }
}

} // namespace

PreviewAtlas buildPreviewAtlas(const std::vector<PreviewImage>& images, u32 cols) {
    PreviewAtlas a;
    a.cells.resize(images.size());

    bool any = false;
    for (const auto& im : images)
        if (im.rgb && im.w > 0 && im.h > 0) { any = true; break; }
    if (!any || images.empty()) return a;

    a.cols = std::max<u32>(1u, std::min<u32>(cols, (u32)images.size()));
    a.rows = ((u32)images.size() + a.cols - 1) / a.cols;
    const u32 stepX = PreviewAtlas::CELL_W + PreviewAtlas::GUTTER * 2;
    const u32 stepY = PreviewAtlas::CELL_H + PreviewAtlas::GUTTER * 2;
    a.width  = a.cols * stepX;
    a.height = a.rows * stepY;

    a.rgba.resize((usize)a.width * a.height * 4);
    for (usize i = 0, n = (usize)a.width * a.height; i < n; ++i) {
        a.rgba[i * 4 + 0] = PreviewAtlas::BG_R;
        a.rgba[i * 4 + 1] = PreviewAtlas::BG_G;
        a.rgba[i * 4 + 2] = PreviewAtlas::BG_B;
        a.rgba[i * 4 + 3] = 255;
    }

    const u32 stride = a.width * 4;
    for (usize i = 0; i < images.size(); ++i) {
        const PreviewImage& im = images[i];
        PreviewCell& c = a.cells[i];
        if (!im.rgb || im.w == 0 || im.h == 0) continue;

        // Вписать, не растягивая и не увеличивая.
        const f32 s = std::min({ 1.f,
                                 (f32)PreviewAtlas::CELL_W / (f32)im.w,
                                 (f32)PreviewAtlas::CELL_H / (f32)im.h });
        const u32 dw = std::clamp<u32>((u32)std::lround((f32)im.w * s), 1u,
                                       PreviewAtlas::CELL_W);
        const u32 dh = std::clamp<u32>((u32)std::lround((f32)im.h * s), 1u,
                                       PreviewAtlas::CELL_H);

        const u32 col = (u32)i % a.cols, row = (u32)i / a.cols;
        const u32 x0 = col * stepX + PreviewAtlas::GUTTER
                     + (PreviewAtlas::CELL_W - dw) / 2;
        const u32 y0 = row * stepY + PreviewAtlas::GUTTER
                     + (PreviewAtlas::CELL_H - dh) / 2;

        boxResize(im.rgb, im.w, im.h,
                  a.rgba.data() + (usize)y0 * stride + (usize)x0 * 4,
                  stride, dw, dh);

        c.has = true;
        c.w = dw; c.h = dh;
        c.aspect = (f32)dw / (f32)dh;
        c.u0 = ((f32)x0 + 0.5f) / (f32)a.width;
        c.v0 = ((f32)y0 + 0.5f) / (f32)a.height;
        c.u1 = ((f32)(x0 + dw) - 0.5f) / (f32)a.width;
        c.v1 = ((f32)(y0 + dh) - 0.5f) / (f32)a.height;
    }
    return a;
}

} // namespace ui
