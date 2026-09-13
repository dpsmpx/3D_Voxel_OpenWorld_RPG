/**
 * @file hud_layout.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#pragma once
#include "../core/types.h"

namespace ui {

/// Геометрия HUD, вынесенная из drawHud.
///
/// Отдельный заголовок нужен затем, что на этом же экране стоят
/// круглые кнопки экранного управления, и их раскладка живёт в другом
/// файле (input/touch_layout.h). Пока каждая сторона знала только свои
/// числа, наложения были видны только на устройстве: «CAM» приходилась
/// ровно на «ATT», «JMP» целиком пряталась под миникартой, «ITM»
/// налезала на пояс предметов. Причём касание в таких местах
/// доставалось HUD — интерфейс проверяется первым, — то есть кнопка не
/// работала вовсе. Теперь оба набора можно сверить на хосте.
struct HudRect { f32 x, y, w, h; };

/// Общий масштаб HUD.
///
/// Все размеры ниже заданы в точках экрана высотой 1080 — на нём игру
/// и смотрели, — а на другом экране пересчитываются пропорционально.
/// Без этого на невысоком экране миникарта, пояс и столбец меню
/// занимают половину площади: семь кнопок по 58 точек это 410 точек
/// из 720.
inline f32 hudScale(f32 screenH) {
    const f32 s = screenH / 1080.f;
    return s < 0.6f ? 0.6f : (s > 1.4f ? 1.4f : s);
}

// ---- правый столбец меню: |||, INV, SKL, ATT, QST, REP, SAV ----
constexpr u32 HUD_MENU_COUNT = 7;
constexpr f32 HUD_MENU_H     = 50.f;
constexpr f32 HUD_MENU_W     = 60.f;
constexpr f32 HUD_MENU_STEP  = 58.f;
constexpr f32 HUD_MENU_TOP   = 12.f;
constexpr f32 HUD_MENU_RIGHT = 20.f;

inline HudRect hudMenuRect(u32 i, f32 screenW, f32 screenH) {
    const f32 k = hudScale(screenH);
    return { screenW - HUD_MENU_W * k - HUD_MENU_RIGHT * k,
             HUD_MENU_TOP * k + (f32)i * HUD_MENU_STEP * k,
             HUD_MENU_W * k, HUD_MENU_H * k };
}

/// Нижняя граница столбца: ниже неё правый край экрана свободен.
inline f32 hudMenuBottom(f32 screenH) {
    const f32 k = hudScale(screenH);
    return (HUD_MENU_TOP + (f32)(HUD_MENU_COUNT - 1) * HUD_MENU_STEP
            + HUD_MENU_H) * k;
}

// ---- миникарта: квадрат в правом нижнем углу вместе с рамкой ----
constexpr f32 MINIMAP_SIZE   = 180.f;
constexpr f32 MINIMAP_MARGIN = 20.f;
constexpr f32 MINIMAP_FRAME  = 4.f;
inline HudRect minimapRect(f32 screenW, f32 screenH) {
    const f32 k = hudScale(screenH);
    const f32 s = MINIMAP_SIZE * k, m = MINIMAP_MARGIN * k, f = MINIMAP_FRAME * k;
    return { screenW - s - m - f, screenH - s - m - f, s + f * 2.f, s + f * 2.f };
}

// ---- пояс предметов: девять ячеек по центру нижнего края ----
constexpr u32 HOTBAR_SLOTS  = 9;
constexpr f32 HOTBAR_SLOT   = 60.f;
constexpr f32 HOTBAR_GAP    = 4.f;
constexpr f32 HOTBAR_BOTTOM = 84.f;
inline HudRect hotbarRect(f32 screenW, f32 screenH) {
    const f32 k = hudScale(screenH);
    const f32 total = ((f32)HOTBAR_SLOTS * (HOTBAR_SLOT + HOTBAR_GAP) - HOTBAR_GAP) * k;
    return { (screenW - total) * 0.5f, screenH - HOTBAR_BOTTOM * k,
             total, HOTBAR_SLOT * k };
}

} // namespace ui
