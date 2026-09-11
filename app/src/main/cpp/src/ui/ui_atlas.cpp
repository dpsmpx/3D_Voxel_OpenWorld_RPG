#include "ui_atlas.h"
#include "font_data.h"

namespace ui {

void UiAtlasData::glyphUv(int ascii, float out[4]) const {
    int c = ascii;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if (c < FONT_FIRST || c > FONT_LAST) c = ' ';
    int idx = c - FONT_FIRST;
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

    // 64 символа в сетке 16×4
    for (int ci = 0; ci < FONT_COUNT; ++ci) {
        int col = ci % 16;
        int row = ci / 16;
        u32 baseX = col * a.cellW;
        u32 baseY = row * a.cellH;

        for (int r = 0; r < FONT_H; ++r) {
            u8 line = FONT[ci][r];
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

    return a;
}

} // namespace ui
