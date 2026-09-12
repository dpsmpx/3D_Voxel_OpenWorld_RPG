/**
 * @file ui_context.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "../config/localization.h"
#include "ui_renderer.h"
#include <functional>
#include <vector>
#include <string>
#include <utility>

namespace ui {

using UiColor = u32;   // RGBA8 packed (R в старшем байте)

constexpr UiColor rgba(u8 r, u8 g, u8 b, u8 a) {
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | (u32)a;
}

constexpr UiColor COL_WHITE  = rgba(255,255,255,255);
constexpr UiColor COL_BLACK  = rgba(  0,  0,  0,255);
constexpr UiColor COL_RED    = rgba(220, 50, 50,255);
constexpr UiColor COL_GREEN  = rgba( 80,200, 80,255);
constexpr UiColor COL_BLUE   = rgba( 60,120,220,255);
constexpr UiColor COL_PURPLE = rgba(160, 80,220,255);
constexpr UiColor COL_YELLOW = rgba(240,220, 60,255);
constexpr UiColor COL_GRAY   = rgba(140,140,140,255);

struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

class UiContext {
public:
    void init(UiRenderer* r, i32 screenW, i32 screenH);
    void setScreen(i32 w, i32 h) { screenW_ = w; screenH_ = h; }

    void beginFrame();
    void endFrame();

    /// ---- Примитивы ----
    void rect(float x, float y, float w, float h, UiColor c);
    void rectOutline(float x, float y, float w, float h, float thickness, UiColor c);
    void text(const std::string& s, float x, float y, float scale, UiColor c);
    /// Прямоугольник из внешнего атласа — того, что подключён через
    /// UiRenderer::attachExternalAtlas. Им рисуется миникарта.
    void image(float x, float y, float w, float h, UiColor tint = COL_WHITE);
    /// Круг и кольцо в пикселях экрана. Экранные кнопки и джойстик
    /// круглые: из прямоугольников они выглядят как лесенка.
    void circle(float cx, float cy, float r, UiColor c, int segments = 24);
    void ring(float cx, float cy, float rInner, float rOuter, UiColor c,
              int segments = 24);
    float textWidth(const std::string& s, float scale) const;
    float textHeight(float scale) const { return 7.f * scale; }

    /// ---- Кнопка ----
    bool button(const std::string& label, Rect r, int interactiveIdx,
                UiColor bg, UiColor fg);

    // ---- Интерактивные области ----
    int  pushInteractiveRect(Rect r, std::function<void()> onTap);
    bool isInteractivePressed(int idx) const;

    /// ---- Маршрутизация тача ----
    bool handleTouch(i32 id, float px, float py, int phase);

    /// ---- Доступ к активному тачу (Phase 13) ----
    /// Позволяет слайдерам и drag-and-drop читать текущую позицию.
    bool  hasActivePointer() const { return pointerActive_; }
    i32   activePointerId()  const { return activePointerId_; }
    float pointerX()         const { return pointerX_; }
    float pointerY()         const { return pointerY_; }
    bool  pointerInside(Rect r) const {
        return pointerActive_ && r.contains(pointerX_, pointerY_);
    }

    /// ---- Доступ к размеру ----
    i32 screenWidth()  const { return screenW_; }
    i32 screenHeight() const { return screenH_; }

private:
    struct Interactive {
        Rect rect;
        std::function<void()> onTap;
        i32 ownerTouch = -1;
        bool pressed = false;
    };

    struct PendingTap {
        float px = 0.f, py = 0.f;
        bool  done = false;
    };

    UiRenderer* r_ = nullptr;
    i32 screenW_ = 1080;
    i32 screenH_ = 1920;

    std::vector<Interactive> interactives_;
    std::vector<PendingTap>  pendingTaps_;

    /// Активный тач — тот, который начал первое касание в этом кадре
    /// и ещё не завершился. Используется для слайдеров и drag.
    bool  pointerActive_ = false;
    i32   activePointerId_ = -1;
    float pointerX_ = 0.f;
    float pointerY_ = 0.f;
};

} // namespace ui
