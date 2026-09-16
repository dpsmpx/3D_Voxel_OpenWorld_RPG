/**
 * @file touch.h
 * @brief Сенсорное управление: джойстик, экранные кнопки, геймпад.
 */
#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>
#include <array>
#include <functional>
#include <vector>
#include <utility>

namespace input {

constexpr u32 MAX_TOUCHES = 10;

/// Одно активное касание экрана.
struct TouchPoint {
    i32       id      = -1;
    glm::vec2 pos{0};          // пиксели, начало координат — левый верх
    glm::vec2 start{0};
    glm::vec2 delta{0};        // смещение за текущий кадр
    f32       startTime = 0;
    f32       duration  = 0;
    bool      active = false;
    bool      uiConsumed = false;   // касание целиком принадлежит UI
};

/// Плавающий джойстик: появляется там, где игрок коснулся своей
/// половины экрана, и живёт до отпускания.
struct VirtualJoystick {
    bool       active = false;
    i32        touchId = -1;
    glm::vec2  center{0};      // пиксели
    glm::vec2  current{0};
    f32        radius = 140.f; // пиксели
    f32        deadzone = 0.15f;
    f32        opacity = 0.6f;

    /// Текущий жест — бег.
    ///
    /// Бег включается ДВОЙНЫМ нажатием: короткий тап, затем сразу
    /// второе нажатие, которое игрок удерживает и ведёт. Так бег
    /// живёт на том же пальце, что и направление, и отдельная кнопка
    /// не нужна — раньше она занимала место на левой половине экрана
    /// и требовала второй руки.
    bool       sprint = false;

    /// Сколько ждать второго нажатия, секунды. Больше — и обычный
    /// повторный заход пальца начинает включать бег сам собой.
    static constexpr f32 DOUBLE_TAP_TIME = 0.30f;
    /// Первое нажатие обязано быть коротким: иначе «повёл и отпустил»
    /// зачтётся за тап, и следующий заход окажется бегом.
    static constexpr f32 TAP_MAX_TIME = 0.25f;

    /// Направление движения в диапазоне [-1, 1] по каждой оси,
    /// с вычтенной мёртвой зоной. Y направлен вперёд (вверх по экрану).
    glm::vec2 axis() const {
        if (!active) return {0, 0};
        glm::vec2 d = current - center;
        d.y = -d.y;                       // экран вниз → мир вперёд
        const f32 len = glm::length(d);
        if (len < 1e-4f) return {0, 0};
        const glm::vec2 dir = d / len;
        f32 mag = glm::min(len / radius, 1.f);
        if (mag < deadzone) return {0, 0};
        mag = (mag - deadzone) / (1.f - deadzone);
        return dir * mag;
    }
};

/// Экранная кнопка. Центр задаётся в нормализованных координатах
/// (-1..1, Y вверх), радиус — в пикселях, поэтому кнопка остаётся
/// одного физического размера на любом соотношении сторон.
struct Button {
    u32        id = 0;
    glm::vec2  center{0};       // NDC, Y вверх
    glm::vec2  userOffset{0};   // сдвиг из настроек (ТЗ 5.2)
    f32        radiusPx = 60.f;
    f32        scale = 1.f;
    bool       pressed = false;
    bool       visible = true;
    i32        touchId = -1;
    /// Короткая подпись для HUD. Указывает на строковый литерал —
    /// кнопки заводятся один раз при старте и живут до выхода.
    const char* label = nullptr;
    std::function<void(u32)> onPress;
    std::function<void(u32)> onRelease;
};

/// Маршрутизатор касаний в UI. phase: 0 — down, 1 — up, 2 — move.
/// Возвращает true, если UI забрал касание себе.
using UiRouterFn = std::function<bool(i32 touchId, float px, float py, int phase)>;

/// TouchInput — разбор MotionEvent в состояние управления.
///
/// Приоритет обработки касания: UI → экранные кнопки → джойстик
/// (своя половина экрана) → камера (противоположная половина).
///
/// Поддерживается Bluetooth-геймпад (ТЗ 5.2): оси и кнопки
/// подмешиваются через injectGamepad*, не мешая сенсорному вводу.
class TouchInput {
public:
    void setViewport(i32 w, i32 h);
    i32  viewportWidth()  const { return screenW_; }
    i32  viewportHeight() const { return screenH_; }

    /// Разбирает один MotionEvent. action — сырое значение
    /// AMotionEvent_getAction, pointerIdx — индекс в событии.
    /// Возвращает true, если событие обработано.
    bool onTouch(i32 action, i32 pointerIdx, i32 pointerId,
                 f32 x, f32 y, f32 timeSec);

    void setCameraSensitivity(f32 s) { cameraSensitivity_ = s; }
    void invertX(bool v)             { invertX_ = v; }
    void invertY(bool v)             { invertY_ = v; }
    void setJoystickLeftHanded(bool leftHanded);
    void setJoystickOpacity(f32 o)   { joystick_.opacity = o; }
    void setJoystickRadius(f32 r)    { joystick_.radius = r; }
    void setJoystickDeadzone(f32 d)  { joystick_.deadzone = d; }

    /// Регистрирует кнопку. centerNdc — (-1..1, Y вверх).
    u32 addButton(glm::vec2 centerNdc, f32 radiusPx,
                  std::function<void(u32)> onPress = nullptr,
                  std::function<void(u32)> onRelease = nullptr);

    bool isButtonHeld(u32 id) const;

    /// Бежит ли игрок. Включается двойным нажатием по джойстику и
    /// держится, пока палец на экране; геймпад подмешивает свою
    /// кнопку. Кнопки бега на экране больше нет.
    bool sprintActive() const { return joystick_.sprint || padSprint_; }

    /// Кнопка бега на геймпаде — отдельным входом, раз экранной
    /// кнопки, через которую её раньше пропускали, не осталось.
    void injectGamepadSprint(bool down) { padSprint_ = down; padActive_ = true; }
    /// Подпись кнопки на экране. Шрифт HUD знает только ASCII до 95,
    /// так что подписи короткие и латиницей.
    void setButtonLabel(u32 id, const char* text);

    /// Где и какого размера кнопка на экране, в пикселях. Нужно HUD,
    /// чтобы нарисовать её ровно там, где она ловит касание: раньше
    /// кнопки существовали только в обработчике ввода, и игрок тыкал
    /// в пустой экран наугад.
    glm::vec2 buttonCenterPx(u32 id) const;
    f32       buttonRadiusPx(u32 id) const;

    // ---- Настройка раскладки (ТЗ 5.2) ----
    /// Сдвиг кнопки относительно штатного места, в NDC.
    /// Базовое положение кнопки. Задаётся при старте и заново после
    /// смены размера окна: раскладка считается в точках экрана, а
    /// хранится в NDC, и при повороте точки другие.
    void      setButtonCenter(u32 id, glm::vec2 centerNdc);
    /// Радиус в точках — тоже пересчитывается вместе с масштабом HUD.
    void      setButtonRadius(u32 id, f32 radiusPx);
    void      setButtonOffset(u32 id, glm::vec2 offsetNdc);
    glm::vec2 buttonOffset(u32 id) const;
    /// Общий масштаб всех кнопок.
    void setButtonScale(f32 s) { buttonScale_ = s < 0.5f ? 0.5f : (s > 2.f ? 2.f : s); }
    f32  buttonScale() const   { return buttonScale_; }
    /// Итоговый центр кнопки с учётом пользовательского сдвига и зеркала.
    glm::vec2 buttonCenter(u32 id) const;
    void setButtonVisible(u32 id, bool v);

    glm::vec2 moveAxis() const;
    glm::vec2 cameraDelta() const;

    /// UI-роутер вызывается первым и может забрать касание.
    void setUiRouter(UiRouterFn fn) { uiRouter_ = std::move(fn); }

    // ---- Режим раскладки (ТЗ 5.2) ----
    /// Пока включён, касание кнопки не запускает её действие, а
    /// перетаскивает её по экрану. Сдвиг сохраняется в настройках.
    void setLayoutMode(bool on);
    bool layoutMode() const { return layoutMode_; }

    /// Вызывается после перетаскивания: id кнопки и её новый сдвиг.
    using LayoutChangedFn = std::function<void(u32 buttonId, glm::vec2 offsetNdc)>;
    void setLayoutCallback(LayoutChangedFn fn) { onLayoutChanged_ = std::move(fn); }

    /// Сбрасывает покадровые накопители. Вызывать в конце кадра.
    void endFrame();

    /// Снимает все касания: при потере фокуса или сворачивании.
    void cancelAll();

    // ---- Геймпад (опционально, ТЗ 5.2) ----
    /// Левый стик: направление движения, [-1, 1], Y вперёд.
    void injectGamepadMove(glm::vec2 axis) { padMove_ = axis; padActive_ = true; }
    /// Правый стик: приращение камеры за кадр, в тех же единицах,
    /// что и свайп, до умножения на чувствительность.
    void injectGamepadLook(glm::vec2 delta) { padLook_ += delta; padActive_ = true; }
    /// Нажатие/отпускание кнопки, привязанной к экранной.
    void injectGamepadButton(u32 buttonId, bool down);
    bool gamepadActive() const { return padActive_; }

    const VirtualJoystick& joystick() const { return joystick_; }
    const std::vector<Button>& buttons() const { return buttons_; }
    const std::array<TouchPoint, MAX_TOUCHES>& touches() const { return touches_; }

private:
    void beginTouch(i32 id, f32 x, f32 y, f32 t);
    void moveTouch (i32 id, f32 x, f32 y, f32 t);
    void endTouch  (i32 id, f32 x, f32 y, f32 t);

    TouchPoint* findTouch(i32 id);
    TouchPoint* allocTouch();

    Button*       findButton(u32 id);
    const Button* findButton(u32 id) const;

    /// Центр кнопки в пикселях с учётом сдвига, масштаба и зеркала.
    glm::vec2 buttonCenterPx(const Button& b) const;
    f32       buttonRadiusPx(const Button& b) const { return b.radiusPx * buttonScale_; }

    /// true, если точка лежит на половине экрана, отведённой джойстику.
    bool onJoystickHalf(f32 x) const;

    i32   screenW_ = 1080, screenH_ = 1920;
    f32   cameraSensitivity_ = 1.f;
    bool  invertX_ = false;
    bool  invertY_ = false;
    bool  joystickLeft_ = true;
    f32   buttonScale_ = 1.f;

    std::array<TouchPoint, MAX_TOUCHES> touches_{};
    VirtualJoystick                     joystick_{};
    std::vector<Button>                 buttons_;
    UiRouterFn                          uiRouter_;

    glm::vec2 cameraDelta_{0};
    u32       nextButtonId_ = 1;

    glm::vec2 padMove_{0};
    glm::vec2 padLook_{0};
    bool      padSprint_ = false;

    /// Когда и где закончился последний КОРОТКИЙ тап по джойстику.
    /// По нему и опознаётся второе нажатие двойного.
    f32       lastTapTime_ = -100.f;
    glm::vec2 lastTapPos_{0};
    /// Начало текущего жеста джойстика — чтобы понять, тап это был
    /// или ведение.
    f32       joyPressTime_ = 0.f;
    glm::vec2 joyPressPos_{0};
    bool      padActive_ = false;

    bool            layoutMode_  = false;
    u32             draggedBtn_  = 0;     ///< кнопка, которую сейчас тащат
    i32             dragTouch_   = -1;
    glm::vec2       dragGrabNdc_{0};      ///< где внутри кнопки взяли
    LayoutChangedFn onLayoutChanged_;
};

} // namespace input
