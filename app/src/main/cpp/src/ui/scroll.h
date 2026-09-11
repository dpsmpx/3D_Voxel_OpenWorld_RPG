#pragma once
#include "../core/types.h"

namespace ui {

// ============================================================
// Скролл для списков (крафт, торговля, квесты, чат).
//
// Хранит смещение в "пикселях контента", максимальное значение,
// скорость для инерции. На вход — только дельты касаний.
// ============================================================
struct Scroll {
    f32 offset      = 0.f;   // текущее смещение в пикселях
    f32 maxOffset   = 0.f;   // верхняя граница (зависит от контента)
    f32 velocity    = 0.f;   // для инерции
    f32 friction    = 6.f;   // коэффициент затухания
    f32 minVelocity = 4.f;   // порог остановки

    // Активное перетаскивание
    bool dragging   = false;
    i32  touchId    = -1;
    f32  dragStartY = 0.f;
    f32  dragStartOffset = 0.f;

    // ---- Установка границ ----
    void setMax(f32 m) {
        maxOffset = (m > 0.f) ? m : 0.f;
        clampOffset();
    }

    void setContentHeight(f32 content, f32 viewport) {
        f32 m = content - viewport;
        setMax(m);
    }

    void clampOffset() {
        if (offset < 0.f)        offset = 0.f;
        if (offset > maxOffset)  offset = maxOffset;
    }

    // ---- Управление перетаскиванием ----
    // Возвращает true, если тач захвачен.
    bool beginDrag(i32 id, f32 y) {
        if (dragging) return false;
        dragging = true;
        touchId  = id;
        dragStartY      = y;
        dragStartOffset = offset;
        velocity = 0.f;
        return true;
    }

    // Обновление позиции. dyPixelsToContent — множитель (обычно 1.0).
    bool updateDrag(i32 id, f32 y, f32 pixelToContent = 1.f) {
        if (!dragging || touchId != id) return false;
        f32 dy = y - dragStartY;
        offset = dragStartOffset - dy * pixelToContent;

        // Мягкое "резиновое" ограничение.
        if (offset < 0.f) {
            offset = offset * 0.4f;
        } else if (offset > maxOffset) {
            offset = maxOffset + (offset - maxOffset) * 0.4f;
        }

        // Инерция по последнему движению.
        velocity = -dy / 0.016f;  // приблизительно
        return true;
    }

    bool endDrag(i32 id) {
        if (!dragging || touchId != id) return false;
        dragging = false;
        touchId  = -1;
        // Пружина возврата, если ушли за границу.
        if (offset < 0.f || offset > maxOffset) {
            offset = (offset < 0.f) ? 0.f : maxOffset;
            velocity = 0.f;
        }
        return true;
    }

    // ---- Инерция / покадровое обновление ----
    void tick(f32 dt) {
        if (dragging) return;
        if (velocity > -minVelocity && velocity < minVelocity) {
            velocity = 0.f;
            return;
        }
        offset += velocity * dt;
        f32 decay = 1.f - friction * dt;
        if (decay < 0.f) decay = 0.f;
        velocity *= decay;

        // Ограничение.
        if (offset < 0.f) {
            offset = 0.f; velocity = 0.f;
        } else if (offset > maxOffset) {
            offset = maxOffset; velocity = 0.f;
        }
    }

    void reset() {
        offset = 0.f; velocity = 0.f; dragging = false;
        touchId = -1; dragStartY = 0.f; dragStartOffset = 0.f;
    }

    // ---- Утилиты для рендера ----
    i32 firstVisibleRow(f32 rowHeight, f32 rowGap) const {
        if (rowHeight + rowGap <= 0.f) return 0;
        return (i32)(offset / (rowHeight + rowGap));
    }

    f32 offsetIntoRow(f32 rowHeight, f32 rowGap) const {
        f32 total = rowHeight + rowGap;
        if (total <= 0.f) return 0.f;
        return offset - (f32)firstVisibleRow(rowHeight, rowGap) * total;
    }
};

} // namespace ui