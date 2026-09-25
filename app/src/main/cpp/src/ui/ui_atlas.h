/**
 * @file ui_atlas.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#pragma once
#include "../core/types.h"
#include <vector>

namespace ui {

/// ---- Раскладка атласа интерфейса ----
///
/// Атлас один на весь интерфейс: шрифт, белый пиксель заливки и
/// значки предметов. Один атлас — один набор дескрипторов и один
/// вызов отрисовки на кадр, сколько бы значков ни было на экране.
///
/// Шрифт — в левом верхнем углу 128×64, как было всегда; под ним
/// сетка значков по ICON_SIZE пикселей.
constexpr u32 UI_ATLAS_W    = 1024;
constexpr u32 UI_ATLAS_H    = 512;
constexpr u32 FONT_REGION_H = 64;
/// Значок предмета, пикселей. Шестьдесят четыре — столько, сколько
/// занимает ячейка пояса на телефоне: выборка ближайшего текселя
/// ни растягивает, ни прореживает значок заметно.
constexpr u32 ICON_SIZE     = 64;
constexpr u32 ICON_COLS     = UI_ATLAS_W / ICON_SIZE;
constexpr u32 ICON_ROWS     = (UI_ATLAS_H - FONT_REGION_H) / ICON_SIZE;
constexpr u32 ICON_CAPACITY = ICON_COLS * ICON_ROWS;

/// Место значка предмета в сетке; −1 — значка нет (номер пуст).
/// Места раздаются по порядку номеров зарегистрированных предметов,
/// и раскладку атласа и чтение из него задаёт одна эта функция.
i32 itemIconSlot(u16 itemId);

/// UV значка предмета: [u0, v0, u1, v1]. false — значка нет.
bool itemIconUv(u16 itemId, float out[4]);

struct UiAtlasData {
    std::vector<u8> pixels;
    u32 width  = UI_ATLAS_W;
    u32 height = UI_ATLAS_H;
    /// Размер ячейки шрифта (с паддингом)
    u32 cellW = 6;
    u32 cellH = 8;
    // UV белого пикселя (центр текселя)
    float whiteU = 0.f;
    float whiteV = 0.f;
    /// UV клетки символа по коду Юникода: [u0,v0,u1,v1].
    /// Латиница и кириллица лежат в одном атласе.
    void glyphUv(int codepoint, float out[4]) const;
};

/// Собирает атлас интерфейса: шрифт 5x7 и значки предметов.
/// Как и атлас блоков, генерируется в памяти без внешних файлов:
/// значки — это модели предметов, отрисованные процессором на
/// прозрачном фоне (render::renderVoxelIcon).
UiAtlasData buildUiAtlas();

} // namespace ui
