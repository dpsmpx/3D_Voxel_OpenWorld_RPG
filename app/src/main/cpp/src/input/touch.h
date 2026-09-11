#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>
#include <array>
#include <functional>
#include <vector>

namespace input {

constexpr u32 MAX_TOUCHES = 10;

struct TouchPoint {
    i32     id      = -1;
    glm::vec2 pos{0};
    glm::vec2 start{0};
    glm::vec2 delta{0};
    f32     startTime = 0;
    f32     duration = 0;
    bool    active = false;
    bool    uiConsumed = false;   // UI owns this touch entirely
};

struct VirtualJoystick {
    bool       active = false;
    i32        touchId = -1;
    glm::vec2  center{0};
    glm::vec2  current{0};
    f32        radius = 120.f;
    f32        deadzone = 0.15f;
    f32        opacity = 0.6f;

    glm::vec2 axis() const {
        if (!active) return {0,0};
        glm::vec2 d = current - center;
        f32 len = glm::length(d);
        if (len < 1e-4f) return {0,0};
        glm::vec2 dir = d / len;
        f32 mag = glm::min(len / radius, 1.f);
        if (mag < deadzone) return {0,0};
        mag = (mag - deadzone) / (1.f - deadzone);
        return dir * mag;
    }
};

struct Button {
    u32        id = 0;
    glm::vec2  center{0};
    f32        radiusPx = 60.f;
    bool       pressed = false;
    i32        touchId = -1;
    std::function<void(u32)> onPress;
    std::function<void(u32)> onRelease;
};

using UiRouterFn = std::function<bool(i32 touchId, float px, float py, int phase)>;

class TouchInput {
public:
    void setViewport(i32 w, i32 h);

    bool onTouch(i32 action, i32 pointerIdx, i32 pointerId,
                 f32 x, f32 y, f32 timeSec);

    void setCameraSensitivity(f32 s) { cameraSensitivity_ = s; }
    void invertY(bool v)             { invertY_ = v; }
    void setJoystickLeftHanded(bool l);
    void setJoystickOpacity(f32 o)   { joystick_.opacity = o; }
    void setJoystickRadius(f32 r)    { joystick_.radius = r; }

    u32 addButton(glm::vec2 centerNdc, f32 radiusPx,
                  std::function<void(u32)> onPress = nullptr,
                  std::function<void(u32)> onRelease = nullptr);

    bool isButtonHeld(u32 id) const;

    glm::vec2 moveAxis() const      { return joystick_.axis(); }
    glm::vec2 cameraDelta() const   { return cameraDelta_ * cameraSensitivity_; }

    // UI router — вызывается первым. Возвращает true, если тач захвачен UI.
    void setUiRouter(UiRouterFn fn) { uiRouter_ = std::move(fn); }

    void endFrame();

    const VirtualJoystick& joystick() const { return joystick_; }
    const std::array<TouchPoint, MAX_TOUCHES>& touches() const { return touches_; }

private:
    void beginTouch(i32 id, f32 x, f32 y, f32 t);
    void moveTouch (i32 id, f32 x, f32 y, f32 t);
    void endTouch  (i32 id, f32 x, f32 y, f32 t);

    TouchPoint* findTouch(i32 id);
    TouchPoint* allocTouch();

    i32   screenW_ = 1080, screenH_ = 1920;
    f32   cameraSensitivity_ = 1.f;
    bool  invertY_ = false;
    bool  joystickLeft_ = true;
    f32   timeSec_ = 0;

    std::array<TouchPoint, MAX_TOUCHES> touches_{};
    VirtualJoystick                     joystick_{};
    std::vector<Button>                 buttons_;
    UiRouterFn                          uiRouter_;

    glm::vec2 cameraDelta_{0};
    u32       nextButtonId_ = 1;
};

} // namespace input