#pragma once
#include "../core/types.h"
#include <vector>

namespace ui {

struct UiAtlasData {
    std::vector<u8> pixels;
    u32 width  = 128;
    u32 height = 64;
    // Размер ячейки шрифта (с паддингом)
    u32 cellW = 6;
    u32 cellH = 8;
    // UV белого пикселя (центр текселя)
    float whiteU = 0.f;
    float whiteV = 0.f;
    // UV клетки символа c (ASCII): возвращает [u0,v0,u1,v1]
    void glyphUv(int ascii, float out[4]) const;
};

UiAtlasData buildUiAtlas();

} // namespace ui
