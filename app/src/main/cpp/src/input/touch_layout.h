/**
 * @file touch_layout.h
 * @brief Круглые кнопки: переходник к общей раскладке экрана.
 */
#pragma once
#include "../core/types.h"
#include "../ui/hud_layout.h"
#include <glm/glm.hpp>

namespace input {

/// Сами числа живут в ui/hud_layout.h — там же, где HUD.
///
/// Раньше раскладка была разрезана надвое: прямоугольники интерфейса
/// в одном файле, круги управления в другом. Каждая сторона знала
/// только свои числа, и наложения обнаруживались только на устройстве:
/// «CAM» приходилась ровно на «ATT», «JMP» целиком пряталась под
/// миникартой. Согласовывать два набора глазами — работа, которой не
/// должно быть.
using ui::PadAnchor;
using ui::PadDef;
using ui::PAD_BUTTONS;

constexpr u32 DEFAULT_BUTTON_COUNT = ui::PAD_BUTTON_COUNT;

/// Центр кнопки в точках экрана.
inline glm::vec2 buttonCenterPx(const ui::HudLayout& L, u32 i) {
    const auto c = L.padButton(i);
    return { c.cx, c.cy };
}

inline f32 buttonRadiusPx(const ui::HudLayout& L, u32 i) {
    return L.padButton(i).r;
}

inline const char* buttonLabel(u32 i) {
    return PAD_BUTTONS[i < DEFAULT_BUTTON_COUNT ? i : 0].label;
}

/// То же в NDC — TouchInput хранит центры именно так: пользовательские
/// сдвиги и зеркальная раскладка для левшей работают в NDC.
inline glm::vec2 buttonCenterNdc(const ui::HudLayout& L, u32 i) {
    const glm::vec2 px = buttonCenterPx(L, i);
    const f32 w = L.width(), h = L.height();
    return {   px.x / w * 2.f - 1.f,
             -(px.y / h * 2.f - 1.f) };
}

} // namespace input
