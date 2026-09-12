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

void UiContext::beginFrame() {
    interactives_.clear();
    pendingTaps_.clear();
    if (r_) r_->beginFrame();
}

void UiContext::endFrame() {
    if (r_) r_->endFrame();
}

// --- NDC-конверсия ---
static inline float toNdcX(float px, i32 W) { return (px / (float)W) * 2.f - 1.f; }
static inline float toNdcY(float py, i32 H) { return 1.f - (py / (float)H) * 2.f; }
static inline float toNdcW(float w,  i32 W) { return (w / (float)W) * 2.f; }
static inline float toNdcH(float h,  i32 H) { return -(h / (float)H) * 2.f; }

void UiContext::rect(float x, float y, float w, float h, UiColor c) {
    if (!r_) return;
    float nx = toNdcX(x, screenW_);
    float ny = toNdcY(y, screenH_);
    float nw = toNdcW(w, screenW_);
    float nh = toNdcH(h, screenH_);
    r_->setAtlas(0);
    r_->pushQuad({ nx, ny }, { nw, nh },
                 r_->whiteU(), r_->whiteV(), r_->whiteU(), r_->whiteV(), c);
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

float UiContext::textWidth(const std::string& s, float scale) const {
    return s.size() * 6.f * scale;
}

int UiContext::pushInteractiveRect(Rect r, std::function<void()> onTap) {
    int idx = (int)interactives_.size();
    Interactive it;
    it.rect = r;
    it.onTap = std::move(onTap);
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
    if (phase == 0) {
        for (int i = (int)interactives_.size() - 1; i >= 0; --i) {
            auto& it = interactives_[i];
            if (it.rect.contains(px, py)) {
                it.ownerTouch = id;
                it.pressed = true;
                return true;
            }
        }
        return false;
    }

    if (phase == 2) {
        bool handled = false;
        for (auto& it : interactives_) {
            if (it.ownerTouch == id) {
                it.pressed = it.rect.contains(px, py);
                handled = true;
            }
        }
        return handled;
    }

    if (phase == 1) {
        for (auto& it : interactives_) {
            if (it.ownerTouch == id) {
                bool inside = it.rect.contains(px, py);
                it.pressed = false;
                it.ownerTouch = -1;
                if (inside) {
                    pendingTaps_.push_back({ px, py, false });
                    if (it.onTap) it.onTap();
                }
                return true;
            }
        }
        return false;
    }

    return false;
}

} // namespace ui
