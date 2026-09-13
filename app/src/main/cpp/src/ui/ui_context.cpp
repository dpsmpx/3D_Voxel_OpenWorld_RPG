/**
 * @file ui_context.cpp
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#include "ui_context.h"
#include "../core/log.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

namespace ui {

void UiContext::init(UiRenderer* r, i32 w, i32 h) {
    r_ = r;
    screenW_ = w;
    screenH_ = h;
    interactives_.reserve(128);
    pendingTaps_.reserve(8);
}

// Один и тот же прямоугольник на соседних кадрах. Числа приходят из
// одних и тех же выражений, поэтому совпадают побитно; допуск нужен
// только на случай, когда размер экрана поменялся между кадрами.
bool UiContext::sameRect(const Rect& a, const Rect& b) {
    auto eq = [](float p, float q) { return std::fabs(p - q) < 0.5f; };
    return eq(a.x, b.x) && eq(a.y, b.y) && eq(a.w, b.w) && eq(a.h, b.h);
}

void UiContext::beginFrame() {
    interactives_.clear();
    pendingTaps_.clear();
    if (r_) r_->beginFrame();
}

void UiContext::endFrame() {
    if (r_) r_->endFrame();
}

// --- NDC-конверсия ---
// Экранные координаты в координаты отсечения.
//
// В Vulkan ось Y направлена вниз: верх экрана это -1, низ +1. Здесь был
// перевод по правилам OpenGL (верх +1), из-за чего весь интерфейс
// оказывался отражён по вертикали, а всё прижатое к низу — джойстик,
// кнопки, пояс предметов — уезжало за верхний край экрана. Касания при
// этом обрабатывались по настоящим пиксельным координатам, и нажимать
// приходилось туда, где ничего не нарисовано.
static inline float toNdcX(float px, i32 W) { return (px / (float)W) * 2.f - 1.f; }
static inline float toNdcY(float py, i32 H) { return (py / (float)H) * 2.f - 1.f; }
static inline float toNdcW(float w,  i32 W) { return (w / (float)W) * 2.f; }
static inline float toNdcH(float h,  i32 H) { return (h / (float)H) * 2.f; }

void UiContext::rect(float x, float y, float w, float h, UiColor c) {
    if (!r_) return;
    float nx = toNdcX(x, screenW_);
    float ny = toNdcY(y, screenH_);
    float nw = toNdcW(w, screenW_);
    float nh = toNdcH(h, screenH_);
    r_->setAtlas(0);
    // -1 — признак сплошной заливки, см. shaders/ui.frag: текстура
    // для неё не нужна вовсе.
    r_->pushQuad({ nx, ny }, { nw, nh }, -1.f, -1.f, -1.f, -1.f, c);
}

void UiContext::image(float x, float y, float w, float h, UiColor tint) {
    if (!r_) return;
    r_->setAtlas(1);
    r_->pushQuad({ toNdcX(x, screenW_), toNdcY(y, screenH_) },
                 { toNdcW(w, screenW_), toNdcH(h, screenH_) },
                 0.f, 0.f, 1.f, 1.f, tint);
    r_->setAtlas(0);
}

void UiContext::rectOutline(float x, float y, float w, float h, float th, UiColor c) {
    rect(x, y, w, th, c);
    rect(x, y + h - th, w, th, c);
    rect(x, y, th, h, c);
    rect(x + w - th, y, th, h, c);
}

void UiContext::text(const std::string& s, float x, float y, float scale, UiColor c) {
    if (!r_) return;
    r_->setAtlas(0);
    float cx = x;
    const float cw = 6.f * scale;

    for (char ch : s) {
        if (ch == '\n') {
            y += 9.f * scale;
            cx = x;
            continue;
        }
        int cc = (unsigned char)ch;
        if (cc >= 'a' && cc <= 'z') cc = cc - 'a' + 'A';
        if (cc < 32 || cc > 95) cc = ' ';

        int idx = cc - 32;
        int col = idx % 16, row = idx / 16;
        float u0 = (float)(col * 6)      / 128.f;
        float v0 = (float)(row * 8)      /  64.f;
        float u1 = (float)(col * 6 + 5)  / 128.f;
        float v1 = (float)(row * 8 + 7)  /  64.f;

        float nx = toNdcX(cx, screenW_);
        float ny = toNdcY(y,  screenH_);
        float nw = toNdcW(5.f * scale, screenW_);
        float nh = toNdcH(7.f * scale, screenH_);
        r_->pushQuad({ nx, ny }, { nw, nh }, u0, v0, u1, v1, c);
        cx += cw;
    }
}

void UiContext::circle(float cx, float cy, float r, UiColor c, int segments) {
    if (!r_ || r <= 0.f) return;
    if (segments < 6) segments = 6;
    if (segments > 64) segments = 64;
    r_->setAtlas(0);
    const glm::vec2 mid{ toNdcX(cx, screenW_), toNdcY(cy, screenH_) };
    const float rx = toNdcW(r, screenW_);
    const float ry = toNdcH(r, screenH_);
    glm::vec2 prev{ mid.x + rx, mid.y };
    for (int i = 1; i <= segments; ++i) {
        const float a = 6.28318530718f * (float)i / (float)segments;
        const glm::vec2 cur{ mid.x + rx * std::cos(a), mid.y + ry * std::sin(a) };
        r_->pushTri(mid, prev, cur, c);
        prev = cur;
    }
}

void UiContext::ring(float cx, float cy, float rInner, float rOuter,
                     UiColor c, int segments)
{
    if (!r_ || rOuter <= rInner) return;
    if (segments < 6) segments = 6;
    if (segments > 64) segments = 64;
    r_->setAtlas(0);
    const glm::vec2 mid{ toNdcX(cx, screenW_), toNdcY(cy, screenH_) };
    const float ix = toNdcW(rInner, screenW_);
    const float iy = toNdcH(rInner, screenH_);
    const float ox = toNdcW(rOuter, screenW_);
    const float oy = toNdcH(rOuter, screenH_);
    glm::vec2 pi{ mid.x + ix, mid.y }, po{ mid.x + ox, mid.y };
    for (int i = 1; i <= segments; ++i) {
        const float a = 6.28318530718f * (float)i / (float)segments;
        const float ca = std::cos(a), sa = std::sin(a);
        const glm::vec2 ci{ mid.x + ix * ca, mid.y + iy * sa };
        const glm::vec2 co{ mid.x + ox * ca, mid.y + oy * sa };
        r_->pushTri(pi, po, co, c);
        r_->pushTri(pi, co, ci, c);
        pi = ci; po = co;
    }
}

float UiContext::textWidth(const std::string& s, float scale) const {
    return s.size() * 6.f * scale;
}

int UiContext::pushInteractiveRect(Rect r, std::function<void()> onTap) {
    int idx = (int)interactives_.size();
    Interactive it;
    it.rect = r;
    it.onTap = std::move(onTap);
    // Нажатие началось на прошлом кадре, а список с тех пор собран
    // заново. Восстанавливаем подсветку по геометрии: прямоугольник
    // одной и той же кнопки от кадра к кадру не меняется.
    if (capture_.active && sameRect(capture_.rect, r)) {
        it.ownerTouch = capture_.touchId;
        it.pressed    = capture_.inside;
    }
    interactives_.push_back(std::move(it));
    return idx;
}

bool UiContext::button(const std::string& label, Rect r, int interactiveIdx,
                       UiColor bg, UiColor fg)
{
    bool pressed = false;
    if (interactiveIdx >= 0 && interactiveIdx < (int)interactives_.size()) {
        pressed = interactives_[interactiveIdx].pressed;
    }

    UiColor fill = pressed ? rgba(180,180,180,255) : bg;
    rect(r.x, r.y, r.w, r.h, fill);
    rectOutline(r.x, r.y, r.w, r.h, 2.f, COL_BLACK);

    float tw = textWidth(label, 2.f);
    float th = textHeight(2.f);
    text(label, r.x + (r.w - tw) * 0.5f, r.y + (r.h - th) * 0.5f, 2.f, fg);

    for (auto& t : pendingTaps_) {
        if (!t.done && r.contains(t.px, t.py)) {
            t.done = true;
            return true;
        }
    }
    return false;
}

bool UiContext::isInteractivePressed(int idx) const {
    if (idx < 0 || idx >= (int)interactives_.size()) return false;
    return interactives_[idx].pressed;
}

bool UiContext::handleTouch(i32 id, float px, float py, int phase) {
    // Обновляем активный указатель.
    if (phase == 0) {  // DOWN
        if (!pointerActive_) {
            pointerActive_ = true;
            activePointerId_ = id;
            pointerX_ = px;
            pointerY_ = py;
        }
    } else if (phase == 2) {  // MOVE
        if (pointerActive_ && activePointerId_ == id) {
            pointerX_ = px;
            pointerY_ = py;
        }
    } else if (phase == 1) {  // UP
        if (pointerActive_ && activePointerId_ == id) {
            pointerX_ = px;
            pointerY_ = py;
            pointerActive_ = false;
            activePointerId_ = -1;
        }
    }

    // Логика интерактивных прямоугольников.
    //
    // Нажатое запоминаем в capture_, а не в самом списке: список
    // пересобирается каждым кадром, а палец держат дольше кадра.
    if (phase == 0) {
        if (capture_.active) return false;   // одно нажатие за раз
        for (int i = (int)interactives_.size() - 1; i >= 0; --i) {
            auto& it = interactives_[i];
            if (!it.rect.contains(px, py)) continue;
            it.ownerTouch   = id;
            it.pressed      = true;
            capture_.active  = true;
            capture_.touchId = id;
            capture_.rect    = it.rect;
            capture_.onTap   = it.onTap;
            capture_.inside  = true;
            return true;
        }
        return false;
    }

    if (phase == 2) {
        if (!capture_.active || capture_.touchId != id) return false;
        capture_.inside = capture_.rect.contains(px, py);
        for (auto& it : interactives_)
            if (sameRect(it.rect, capture_.rect)) it.pressed = capture_.inside;
        return true;
    }

    if (phase == 1) {
        if (!capture_.active || capture_.touchId != id) return false;
        const bool inside = capture_.rect.contains(px, py);
        auto onTap = std::move(capture_.onTap);
        for (auto& it : interactives_)
            if (sameRect(it.rect, capture_.rect)) { it.pressed = false; it.ownerTouch = -1; }
        capture_ = Capture{};
        if (inside) {
            pendingTaps_.push_back({ px, py, false });
            if (onTap) onTap();
        }
        return true;
    }

    return false;
}

} // namespace ui
