/**
 * @file ui_types.h
 * @brief Базовые типы интерфейса: упакованный цвет и прямоугольник.
 */
#pragma once
#include "../core/types.h"

namespace ui {

using UiColor = u32;   // RGBA8 packed (R в старшем байте)

constexpr UiColor rgba(u8 r, u8 g, u8 b, u8 a) {
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | (u32)a;
}

/// Тот же цвет с другой непрозрачностью. Нужен теме: один токен
/// обслуживает и обычное состояние, и выключенное.
constexpr UiColor withAlpha(UiColor c, u8 a) {
    return (c & 0xFFFFFF00u) | (u32)a;
}

/// Прямоугольник в точках экрана. Начало координат — левый верх.
///
/// Это единственный источник геометрии для любого элемента: тот же
/// Rect, по которому элемент рисуется, ловит и касание. Расхождение
/// между «где нарисовано» и «где ловится» — ошибка по определению.
struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

} // namespace ui
