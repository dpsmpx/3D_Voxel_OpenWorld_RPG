/**
 * @file touch_layout.h
 * @brief Сенсорное управление: джойстик, экранные кнопки, геймпад.
 */
#pragma once
#include "../core/types.h"
#include "../ui/hud_layout.h"
#include <glm/glm.hpp>

namespace input {

/// К какому углу экрана привязана кнопка.
///
/// Раньше положения задавались в NDC, а радиусы — в пикселях. На
/// широком экране NDC-доля не совпадает с пиксельным отступом HUD, и
/// кнопки наезжали на интерфейс тем сильнее, чем шире экран. Привязка
/// к углу в точках снимает зависимость от соотношения сторон целиком:
/// от края до кнопки всегда одно и то же расстояние.
enum class Anchor : u8 { BottomLeft, BottomRight, TopRight };

/// Кнопка экранного управления по умолчанию. dx, dy — отступ от угла
/// внутрь экрана в точках при высоте 1080; радиус там же.
struct ButtonLayout {
    Anchor      anchor;
    f32         dx, dy;
    f32         radiusPx;
    const char* label;
};

/// Порядок обязан совпадать с config::ButtonSlot: по нему
/// раскладываются сохранённые пользовательские сдвиги.
inline constexpr ButtonLayout DEFAULT_BUTTONS[] = {
    { Anchor::BottomRight, 323.f, 205.f, 100.f, "ATK" },
    { Anchor::BottomRight, 115.f, 378.f,  60.f, "FIN" },
    // Прыжок стоял в самом углу — целиком под миникартой, и на экране
    // его не было видно вовсе. Ушёл выше и левее, к остальным.
    { Anchor::BottomRight, 260.f, 380.f,  85.f, "JMP" },
    { Anchor::BottomLeft,  138.f, 432.f,  70.f, "RUN" },
    { Anchor::BottomRight, 553.f, 378.f,  70.f, "DIG" },
    { Anchor::BottomRight, 738.f, 205.f,  80.f, "PUT" },
    { Anchor::BottomLeft,  553.f, 205.f,  70.f, "USE" },
    // Налезала на пояс предметов: пояс по центру, а кнопка была
    // привязана к левому краю, и на узком экране они сходились.
    { Anchor::BottomLeft,  570.f,  76.f,  55.f, "ITM" },
    // Стояла в правом верхнем углу, ровно под столбцом меню HUD.
    // Свободная полоса — ниже столбца.
    { Anchor::TopRight,     92.f, 470.f,  55.f, "CAM" },
};

constexpr u32 DEFAULT_BUTTON_COUNT =
    (u32)(sizeof(DEFAULT_BUTTONS) / sizeof(DEFAULT_BUTTONS[0]));

/// Центр кнопки в точках экрана. Масштаб — общий с HUD, иначе на
/// невысоком экране кнопки останутся прежними, а интерфейс усохнет.
inline glm::vec2 buttonCenterPx(const ButtonLayout& L, f32 screenW, f32 screenH) {
    const f32 k = ui::hudScale(screenH);
    const f32 dx = L.dx * k, dy = L.dy * k;
    switch (L.anchor) {
        case Anchor::BottomLeft:  return { dx,           screenH - dy };
        case Anchor::BottomRight: return { screenW - dx, screenH - dy };
        case Anchor::TopRight:    return { screenW - dx, dy };
    }
    return { screenW * 0.5f, screenH * 0.5f };
}

inline f32 buttonRadiusPx(const ButtonLayout& L, f32 screenH) {
    return L.radiusPx * ui::hudScale(screenH);
}

/// То же в NDC — TouchInput хранит центры именно так: пользовательские
/// сдвиги и зеркальная раскладка для левшей работают в NDC.
inline glm::vec2 buttonCenterNdc(const ButtonLayout& L, f32 screenW, f32 screenH) {
    const glm::vec2 px = buttonCenterPx(L, screenW, screenH);
    return {   px.x / screenW * 2.f - 1.f,
             -(px.y / screenH * 2.f - 1.f) };
}

} // namespace input
