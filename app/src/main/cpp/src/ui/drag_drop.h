#pragma once
#include "../core/types.h"
#include "../items/item_stack.h"
#include <glm/glm.hpp>

namespace ui {

// ============================================================
// Состояние drag-and-drop для инвентаря / крафта / торговли.
//
// Использование:
//   1) При тапе на слот с предметом вызвать begin() с полным
//      стеком, либо с половинным (long-tap).
//   2) Каждый кадр вызывать update(pos, now) — обновляет позицию
//      курсора для рендера.
//   3) При отпускании пальца над drop-target вызвать dropTo(idx)
//      на стороне вызывающего, передав фактическую операцию
//      в Inventory, затем end().
//   4) При отпускании над пустотой — cancel() (вернуть в исходный слот).
//
// Модуль НЕ владеет инвентарём: вся логика перемещения делается
// снаружи, drag_drop хранит только визуальное состояние и позицию.
// ============================================================
struct DragDrop {
    bool             active    = false;
    i32              touchId   = -1;
    u32              fromSlot  = 0;
    items::ItemStack stack{};
    glm::vec2        pos{0.f, 0.f};
    glm::vec2        startPos{0.f, 0.f};
    f32              elapsed   = 0.f;

    // Разбит ли стек на половину (long-tap без движения).
    bool             isSplit   = false;

    // Отменён ли drag (вернуть в исходный слот).
    bool             cancelled = false;

    // Параметры
    static constexpr f32 LONG_TAP_TIME  = 0.45f;
    static constexpr f32 DRAG_THRESHOLD = 24.f;   // пикселей

    // Начать перетаскивание. stack — то, что тащим (полный или
    // половинный). sourceSlot — откуда визуально убрать.
    void begin(i32 id, u32 sourceSlot, glm::vec2 p, const items::ItemStack& s) {
        active    = true;
        touchId   = id;
        fromSlot  = sourceSlot;
        stack     = s;
        pos       = p;
        startPos  = p;
        elapsed   = 0.f;
        isSplit   = false;
        cancelled = false;
    }

    // Обновление позиции и таймера.
    void update(glm::vec2 p, f32 dt) {
        if (!active) return;
        pos = p;
        elapsed += dt;
    }

    // Завершить успешным дропом.
    void end() {
        active    = false;
        touchId   = -1;
        fromSlot  = 0;
        stack.clear();
        isSplit   = false;
        cancelled = false;
        elapsed   = 0.f;
    }

    // Завершить отменой.
    void cancel() {
        if (!active) return;
        cancelled = true;
        // Реальное возвращение стека делает вызывающий код.
        active   = false;
        touchId  = -1;
    }

    void clear() {
        active    = false;
        touchId   = -1;
        fromSlot  = 0;
        stack.clear();
        isSplit   = false;
        cancelled = false;
        elapsed   = 0.f;
    }

    // ---- Диагностика ----
    bool isDragging() const {
        if (!active) return false;
        glm::vec2 d = pos - startPos;
        return glm::dot(d, d) >= DRAG_THRESHOLD * DRAG_THRESHOLD;
    }

    bool isLongTapReached() const {
        return active && !isSplit && elapsed >= LONG_TAP_TIME;
    }

    bool matchesTouch(i32 id) const {
        return active && touchId == id;
    }
};

} // namespace ui