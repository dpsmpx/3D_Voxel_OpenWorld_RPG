/**
 * @file ui_atlas.cpp
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#include "ui_atlas.h"
#include "font_data.h"
#include "../items/item_def.h"
#include "../items/item_models.h"
#include "../render/voxel_model.h"
#include <vector>

namespace ui {

namespace {

/// Места значков по номерам предметов, по порядку регистрации.
const std::vector<i16>& iconSlots() {
    static const std::vector<i16> slots = [] {
        std::vector<i16> t(items::ITEM_MAX_DEFS, (i16)-1);
        i16 next = 0;
        for (u16 id = 1; id < items::ITEM_MAX_DEFS; ++id) {
            if (!items::items().get(id).name) continue;
            if ((u32)next >= ICON_CAPACITY) break;
            t[id] = next++;
        }
        return t;
    }();
    return slots;
}

} // namespace

i32 itemIconSlot(u16 itemId) {
    const auto& t = iconSlots();
    return itemId < t.size() ? (i32)t[itemId] : -1;
}

bool itemIconUv(u16 itemId, float out[4]) {
    const i32 slot = itemIconSlot(itemId);
    if (slot < 0) return false;
    const u32 x = ((u32)slot % ICON_COLS) * ICON_SIZE;
    const u32 y = FONT_REGION_H + ((u32)slot / ICON_COLS) * ICON_SIZE;
    out[0] = (f32)x / (f32)UI_ATLAS_W;
    out[1] = (f32)y / (f32)UI_ATLAS_H;
    out[2] = (f32)(x + ICON_SIZE) / (f32)UI_ATLAS_W;
    out[3] = (f32)(y + ICON_SIZE) / (f32)UI_ATLAS_H;
    return true;
}

void UiAtlasData::glyphUv(int codepoint, float out[4]) const {
    int idx = glyphIndex((u32)codepoint);
    if (idx < 0) idx = (int)' ' - FONT_FIRST;
    int col = idx % 16;
    int row = idx / 16;

    float u0 = (float)(col * cellW) / (float)width;
    float v0 = (float)(row * cellH) / (float)height;
    // Ограничиваем области глифа 5×7 внутри клетки 6×8
    float u1 = (float)(col * cellW + FONT_W) / (float)width;
    float v1 = (float)(row * cellH + FONT_H) / (float)height;
    out[0] = u0; out[1] = v0; out[2] = u1; out[3] = v1;
}

UiAtlasData buildUiAtlas() {
    UiAtlasData a;
    a.pixels.assign(a.width * a.height * 4, 0);

    // 64 латинских плюс 33 кириллических — 97 клеток в сетке по 16.
    // Атлас 128x64 даёт 128 клеток, так что расширять его не пришлось.
    for (int ci = 0; ci < GLYPH_COUNT; ++ci) {
        int col = ci % 16;
        int row = ci / 16;
        u32 baseX = col * a.cellW;
        u32 baseY = row * a.cellH;

        for (int r = 0; r < FONT_H; ++r) {
            u8 line = (ci < FONT_COUNT) ? FONT[ci][r]
                                        : FONT_CYR[ci - FONT_COUNT][r];
            for (int x = 0; x < FONT_W; ++x) {
                bool on = (line >> (4 - x)) & 1;
                u32 px = baseX + x;
                u32 py = baseY + r;
                u32 idx = (py * a.width + px) * 4;
                a.pixels[idx + 0] = 255;
                a.pixels[idx + 1] = 255;
                a.pixels[idx + 2] = 255;
                a.pixels[idx + 3] = on ? 255 : 0;
            }
        }
    }

    // Белый пиксель для заливки прямоугольников — в клетке (16, 0)
    // (позиция 96..97, 0..1). Возьмём (96, 0).
    {
        u32 px = 16 * a.cellW;   // 96
        u32 py = 0;
        u32 idx = (py * a.width + px) * 4;
        a.pixels[idx + 0] = 255;
        a.pixels[idx + 1] = 255;
        a.pixels[idx + 2] = 255;
        a.pixels[idx + 3] = 255;
        a.whiteU = (float(px) + 0.5f) / (float)a.width;
        a.whiteV = (float(py) + 0.5f) / (float)a.height;
    }

    // Значки предметов: модель каждого предмета, отрисованная на
    // прозрачном фоне прямо в свою клетку атласа.
    for (u16 id = 1; id < items::ITEM_MAX_DEFS; ++id) {
        const i32 slot = itemIconSlot(id);
        if (slot < 0) continue;
        const u32 x = ((u32)slot % ICON_COLS) * ICON_SIZE;
        const u32 y = FONT_REGION_H + ((u32)slot / ICON_COLS) * ICON_SIZE;
        render::renderVoxelIcon(items::itemModel(id), ICON_SIZE,
                                &a.pixels[((usize)y * a.width + x) * 4], a.width * 4);
    }

    return a;
}

} // namespace ui
