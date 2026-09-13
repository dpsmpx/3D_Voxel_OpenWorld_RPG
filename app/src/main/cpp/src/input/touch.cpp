/**
 * @file touch.cpp
 * @brief Сенсорное управление: джойстик, экранные кнопки, геймпад.
 */
#include "touch.h"
#include <android/input.h>
#include <algorithm>
#include <cmath>
#include <functional>

namespace input {

// ============================================================
// Геометрия
// ============================================================

void TouchInput::setViewport(i32 w, i32 h) {
    screenW_ = w > 0 ? w : 1;
    screenH_ = h > 0 ? h : 1;
}

glm::vec2 TouchInput::buttonCenterPx(const Button& b) const {
    glm::vec2 ndc = b.center + b.userOffset;
    // Режим для левши — зеркалим раскладку по горизонтали.
    if (!joystickLeft_) ndc.x = -ndc.x;
    return {
        ( ndc.x * 0.5f + 0.5f) * (f32)screenW_,
        (-ndc.y * 0.5f + 0.5f) * (f32)screenH_,   // NDC Y вверх, экран Y вниз
    };
}

bool TouchInput::onJoystickHalf(f32 x) const {
    const f32 mid = (f32)screenW_ * 0.5f;
    return joystickLeft_ ? (x < mid) : (x > mid);
}

// ============================================================
// Пул касаний
// ============================================================

TouchPoint* TouchInput::findTouch(i32 id) {
    for (auto& t : touches_)
        if (t.active && t.id == id) return &t;
    return nullptr;
}

TouchPoint* TouchInput::allocTouch() {
    for (auto& t : touches_)
        if (!t.active) return &t;
    return nullptr;   // больше MAX_TOUCHES пальцев — лишние игнорируем
}

void TouchInput::cancelAll() {
    for (auto& t : touches_) {
        if (t.active && t.uiConsumed && uiRouter_) uiRouter_(t.id, t.pos.x, t.pos.y, 1);
        t = TouchPoint{};
    }
    for (auto& b : buttons_) {
        if (b.pressed) {
            b.pressed = false;
            b.touchId = -1;
            if (b.onRelease) b.onRelease(b.id);
        }
    }
    joystick_.active  = false;
    joystick_.touchId = -1;
    cameraDelta_ = {0, 0};
}

// ============================================================
// Кнопки
// ============================================================

u32 TouchInput::addButton(glm::vec2 centerNdc, f32 radiusPx,
                          std::function<void(u32)> onPress,
                          std::function<void(u32)> onRelease)
{
    Button b;
    b.id        = nextButtonId_++;
    b.center    = centerNdc;
    b.radiusPx  = radiusPx;
    b.onPress   = std::move(onPress);
    b.onRelease = std::move(onRelease);
    buttons_.push_back(std::move(b));
    return buttons_.back().id;
}

Button* TouchInput::findButton(u32 id) {
    for (auto& b : buttons_) if (b.id == id) return &b;
    return nullptr;
}

const Button* TouchInput::findButton(u32 id) const {
    for (const auto& b : buttons_) if (b.id == id) return &b;
    return nullptr;
}

bool TouchInput::isButtonHeld(u32 id) const {
    const Button* b = findButton(id);
    return b && b->pressed;
}

void TouchInput::setButtonCenter(u32 id, glm::vec2 centerNdc) {
    if (Button* b = findButton(id)) b->center = centerNdc;
}

void TouchInput::setButtonRadius(u32 id, f32 radiusPx) {
    if (Button* b = findButton(id)) b->radiusPx = radiusPx;
}

void TouchInput::setButtonOffset(u32 id, glm::vec2 offsetNdc) {
    if (Button* b = findButton(id)) b->userOffset = offsetNdc;
}

glm::vec2 TouchInput::buttonOffset(u32 id) const {
    const Button* b = findButton(id);
    return b ? b->userOffset : glm::vec2{0, 0};
}

glm::vec2 TouchInput::buttonCenter(u32 id) const {
    const Button* b = findButton(id);
    if (!b) return {0, 0};
    glm::vec2 ndc = b->center + b->userOffset;
    if (!joystickLeft_) ndc.x = -ndc.x;
    return ndc;
}

void TouchInput::setButtonLabel(u32 id, const char* text) {
    if (Button* b = findButton(id)) b->label = text;
}

glm::vec2 TouchInput::buttonCenterPx(u32 id) const {
    const Button* b = findButton(id);
    return b ? buttonCenterPx(*b) : glm::vec2{0, 0};
}

f32 TouchInput::buttonRadiusPx(u32 id) const {
    const Button* b = findButton(id);
    return b ? buttonRadiusPx(*b) : 0.f;
}

void TouchInput::setButtonVisible(u32 id, bool v) {
    if (Button* b = findButton(id)) b->visible = v;
}

void TouchInput::setLayoutMode(bool on) {
    if (layoutMode_ == on) return;
    layoutMode_ = on;
    draggedBtn_ = 0;
    dragTouch_  = -1;
    // Выходя из режима, гасим «залипшие» нажатия.
    for (auto& b : buttons_) {
        if (b.pressed) {
            b.pressed = false;
            b.touchId = -1;
        }
    }
}

void TouchInput::setJoystickLeftHanded(bool leftHanded) {
    if (joystickLeft_ == leftHanded) return;
    joystickLeft_ = leftHanded;
    // Активный джойстик остался бы на чужой половине — сбрасываем.
    joystick_.active  = false;
    joystick_.touchId = -1;
}

// ============================================================
// Разбор MotionEvent
// ============================================================

bool TouchInput::onTouch(i32 action, i32 pointerIdx, i32 pointerId,
                         f32 x, f32 y, f32 timeSec)
{
    const i32 masked  = action & AMOTION_EVENT_ACTION_MASK;
    const i32 changed = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                        >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    switch (masked) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            // В событии перечислены все пальцы, но опустился только один.
            if (pointerIdx != changed) return false;
            beginTouch(pointerId, x, y, timeSec);
            return true;

        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (pointerIdx != changed) return false;
            endTouch(pointerId, x, y, timeSec);
            return true;

        case AMOTION_EVENT_ACTION_MOVE:
            // MOVE относится ко всем пальцам сразу.
            moveTouch(pointerId, x, y, timeSec);
            return true;

        case AMOTION_EVENT_ACTION_CANCEL:
            cancelAll();
            return true;

        default:
            return false;
    }
}

void TouchInput::beginTouch(i32 id, f32 x, f32 y, f32 t) {
    TouchPoint* tp = findTouch(id);
    if (!tp) tp = allocTouch();
    if (!tp) return;

    tp->id        = id;
    tp->pos       = {x, y};
    tp->start     = {x, y};
    tp->delta     = {0, 0};
    tp->startTime = t;
    tp->duration  = 0.f;
    tp->active    = true;
    tp->uiConsumed = false;

    // 1. UI имеет приоритет над игровым вводом.
    if (uiRouter_ && uiRouter_(id, x, y, 0)) {
        tp->uiConsumed = true;
        return;
    }

    // 2. Экранные кнопки. Расстояние считаем в пикселях, поэтому
    //    попадание не зависит от соотношения сторон.
    for (auto& b : buttons_) {
        if (!b.visible) continue;
        const glm::vec2 c = buttonCenterPx(b);
        const f32 r = buttonRadiusPx(b);
        if (glm::length(glm::vec2(x, y) - c) > r) continue;

        // В режиме раскладки касание кнопки её перетаскивает,
        // а не запускает действие.
        if (layoutMode_) {
            draggedBtn_ = b.id;
            dragTouch_  = id;
            dragGrabNdc_ = {
                 (x / (f32)screenW_) * 2.f - 1.f,
                -((y / (f32)screenH_) * 2.f - 1.f),
            };
            dragGrabNdc_ -= buttonCenter(b.id);
            return;
        }

        if (b.pressed) continue;
        b.pressed = true;
        b.touchId = id;
        if (b.onPress) b.onPress(b.id);
        return;
    }

    // 3. Джойстик появляется под пальцем на своей половине экрана.
    if (onJoystickHalf(x) && !joystick_.active) {
        joystick_.active  = true;
        joystick_.touchId = id;
        joystick_.center  = {x, y};
        joystick_.current = {x, y};
    }
}

void TouchInput::moveTouch(i32 id, f32 x, f32 y, f32 t) {
    TouchPoint* tp = findTouch(id);
    if (!tp) return;

    const glm::vec2 prev = tp->pos;
    tp->pos      = {x, y};
    tp->delta    = tp->pos - prev;
    tp->duration = t - tp->startTime;

    if (tp->uiConsumed) {
        if (uiRouter_) uiRouter_(id, x, y, 2);
        return;
    }

    // Перетаскивание кнопки в режиме раскладки.
    if (layoutMode_ && draggedBtn_ && dragTouch_ == id) {
        Button* b = findButton(draggedBtn_);
        if (b) {
            glm::vec2 ndc{
                 (x / (f32)screenW_) * 2.f - 1.f,
                -((y / (f32)screenH_) * 2.f - 1.f),
            };
            ndc -= dragGrabNdc_;
            // Раскладка хранится до зеркалирования под левшу.
            glm::vec2 base = b->center;
            if (!joystickLeft_) { ndc.x = -ndc.x; }
            glm::vec2 off = ndc - base;
            // Не даём утащить кнопку за край экрана.
            off.x = glm::clamp(off.x, -1.6f, 1.6f);
            off.y = glm::clamp(off.y, -1.6f, 1.6f);
            b->userOffset = off;
        }
        return;
    }

    if (joystick_.active && joystick_.touchId == id) {
        glm::vec2 d = glm::vec2(x, y) - joystick_.center;
        const f32 len = glm::length(d);
        if (len > joystick_.radius) d = d / len * joystick_.radius;
        joystick_.current = joystick_.center + d;
        return;
    }

    // Палец, удерживающий кнопку, камеру не вращает.
    for (const auto& b : buttons_)
        if (b.pressed && b.touchId == id) return;

    // Камера — свайп по половине экрана, противоположной джойстику.
    if (!onJoystickHalf(x)) {
        // Нормируем на высоту экрана: одинаковый свайп пальцем даёт
        // одинаковый поворот на любом разрешении.
        glm::vec2 d = tp->delta / (f32)screenH_;
        if (invertX_) d.x = -d.x;
        if (invertY_) d.y = -d.y;
        cameraDelta_ += d;
    }
}

void TouchInput::endTouch(i32 id, f32 x, f32 y, f32 t) {
    TouchPoint* tp = findTouch(id);
    if (!tp) return;
    tp->duration = t - tp->startTime;

    if (layoutMode_ && draggedBtn_ && dragTouch_ == id) {
        if (onLayoutChanged_) {
            if (const Button* b = findButton(draggedBtn_))
                onLayoutChanged_(b->id, b->userOffset);
        }
        draggedBtn_ = 0;
        dragTouch_  = -1;
        *tp = TouchPoint{};
        return;
    }

    if (tp->uiConsumed) {
        if (uiRouter_) uiRouter_(id, x, y, 1);
    } else {
        if (joystick_.active && joystick_.touchId == id) {
            joystick_.active  = false;
            joystick_.touchId = -1;
        }
        for (auto& b : buttons_) {
            if (b.pressed && b.touchId == id) {
                b.pressed = false;
                b.touchId = -1;
                if (b.onRelease) b.onRelease(b.id);
            }
        }
    }

    *tp = TouchPoint{};
}

// ============================================================
// Выход
// ============================================================

glm::vec2 TouchInput::moveAxis() const {
    const glm::vec2 stick = joystick_.axis();
    if (glm::length(stick) > 1e-4f) return stick;
    // Геймпад работает, пока не тронут экранный джойстик.
    return padMove_;
}

glm::vec2 TouchInput::cameraDelta() const {
    return (cameraDelta_ + padLook_) * cameraSensitivity_;
}

void TouchInput::injectGamepadButton(u32 buttonId, bool down) {
    padActive_ = true;
    Button* b = findButton(buttonId);
    if (!b) return;
    if (down && !b->pressed) {
        b->pressed = true;
        b->touchId = -1;
        if (b->onPress) b->onPress(b->id);
    } else if (!down && b->pressed && b->touchId == -1) {
        b->pressed = false;
        if (b->onRelease) b->onRelease(b->id);
    }
}

void TouchInput::endFrame() {
    cameraDelta_ = {0, 0};
    padLook_     = {0, 0};
    for (auto& t : touches_) t.delta = {0, 0};
}

} // namespace input
