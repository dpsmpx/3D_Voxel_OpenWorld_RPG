void TouchInput::beginTouch(i32 id, f32 x, f32 y, f32 t) {
    TouchPoint* tp = findTouch(id);
    if (!tp) tp = allocTouch();
    if (!tp) return;
    tp->id = id;
    tp->pos = {x, y};
    tp->start = {x, y};
    tp->delta = {0,0};
    tp->startTime = t;
    tp->duration = 0;
    tp->active = true;
    tp->uiConsumed = false;

    // 1. UI получает приоритет
    if (uiRouter_ && uiRouter_(id, x, y, 0)) {
        tp->uiConsumed = true;
        return;
    }

    // 2. Кнопки игровые
    f32 nx = (x / screenW_) * 2.f - 1.f;
    f32 ny = (y / screenH_) * 2.f - 1.f;
    for (auto& b : buttons_) {
        glm::vec2 d = glm::vec2(nx, ny) - b.center;
        f32 nxRadius = b.radiusPx / (f32)screenW_ * 2.f;
        if (glm::length(d) < nxRadius) {
            b.pressed = true;
            b.touchId = id;
            if (b.onPress) b.onPress(b.id);
            return;
        }
    }

    // 3. Джойстик
    bool leftSide = joystickLeft_ ? (x < screenW_ * 0.5f) : (x > screenW_ * 0.5f);
    if (leftSide && !joystick_.active) {
        joystick_.active = true;
        joystick_.touchId = id;
        joystick_.center = {x, y};
        joystick_.current = {x, y};
    }
}

void TouchInput::moveTouch(i32 id, f32 x, f32 y, f32 t) {
    TouchPoint* tp = findTouch(id);
    if (!tp) return;
    glm::vec2 prev = tp->pos;
    tp->pos = {x, y};
    tp->delta = tp->pos - prev;
    tp->duration = t - tp->startTime;

    if (tp->uiConsumed) {
        if (uiRouter_) uiRouter_(id, x, y, 2);
        return;
    }

    if (joystick_.active && joystick_.touchId == id) {
        glm::vec2 d = {x, y} - joystick_.center;
        f32 len = glm::length(d);
        if (len > joystick_.radius) d = d / len * joystick_.radius;
        joystick_.current = joystick_.center + d;
        return;
    }

    // Камера — если тач не в левой (джойстик) половине
    bool leftHalf = joystickLeft_ ? (x < screenW_ * 0.5f) : (x > screenW_ * 0.5f);
    if (!leftHalf) {
        cameraDelta_ += tp->delta * 0.005f;
        if (invertY_) cameraDelta_.y = -cameraDelta_.y;
    }
}

void TouchInput::endTouch(i32 id, f32 x, f32 y, f32 t) {
    TouchPoint* tp = findTouch(id);
    if (!tp) return;

    if (tp->uiConsumed) {
        if (uiRouter_) uiRouter_(id, x, y, 1);
    } else {
        if (joystick_.active && joystick_.touchId == id) {
            joystick_.active = false;
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

    tp->active = false;
    tp->id = -1;
    tp->uiConsumed = false;
}