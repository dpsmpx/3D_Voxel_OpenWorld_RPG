#include "slider.h"
#include "../core/log.h"
#include <algorithm>
#include <cstdio>

namespace ui {

bool sliderWidget(UiContext& ui,
                  Rect r,
                  float* value,
                  float minV,
                  float maxV,
                  const std::string& label,
                  float step,
                  std::function<void(float)> onChanged)
{
    if (!value) return false;

    float range = maxV - minV;
    if (range < 0.0001f) range = 1.f;

    float norm = (*value - minV) / range;
    if (norm < 0.f) norm = 0.f;
    if (norm > 1.f) norm = 1.f;

    float labelW = label.empty() ? 0.f : ui.textWidth(label, 1.6f);
    float trackX = r.x + labelW + (label.empty() ? 0.f : 12.f);
    float trackW = r.w - labelW - (label.empty() ? 0.f : 12.f);
    float trackH = 12.f;
    float trackY = r.y + (r.h - trackH) * 0.5f;
    if (trackW < 20.f) trackW = 20.f;

    Rect hit{ trackX, trackY - 12.f, trackW, trackH + 24.f };
    int idx = ui.pushInteractiveRect(hit, nullptr);
    bool pressed = ui.isInteractivePressed(idx);

    bool changed = false;

    // Обработка drag: если палец на слайдере — считаем значение.
    if (pressed && ui.hasActivePointer()) {
        float px = ui.pointerX();
        float local = (px - trackX) / trackW;
        if (local < 0.f) local = 0.f;
        if (local > 1.f) local = 1.f;

        float newV = minV + local * range;
        if (step > 0.f) {
            newV = minV + std::round((newV - minV) / step) * step;
            if (newV > maxV) newV = maxV;
            if (newV < minV) newV = minV;
        }
        if (newV != *value) {
            *value = newV;
            changed = true;
            if (onChanged) onChanged(newV);
        }
        norm = (*value - minV) / range;
        if (norm < 0.f) norm = 0.f;
        if (norm > 1.f) norm = 1.f;
    }

    // ---- Рендер ----
    ui.rect(trackX, trackY, trackW, trackH, rgba(40, 40, 40, 255));
    ui.rectOutline(trackX, trackY, trackW, trackH, 2.f, COL_BLACK);

    float fillW = trackW * norm;
    ui.rect(trackX, trackY, fillW, trackH, rgba(120, 180, 240, 255));

    float thumbW = 22.f;
    float thumbX = trackX + fillW - thumbW * 0.5f;
    if (thumbX < trackX - thumbW * 0.5f) thumbX = trackX - thumbW * 0.5f;
    if (thumbX > trackX + trackW - thumbW * 0.5f)
        thumbX = trackX + trackW - thumbW * 0.5f;

    ui.rect(thumbX, trackY - 6.f, thumbW, trackH + 12.f,
            pressed ? rgba(255, 240, 160, 255) : rgba(220, 220, 220, 255));
    ui.rectOutline(thumbX, trackY - 6.f, thumbW, trackH + 12.f, 2.f, COL_BLACK);

    if (!label.empty()) {
        ui.text(label, r.x, r.y + (r.h - ui.textHeight(1.6f)) * 0.5f,
                1.6f, COL_WHITE);
    }

    {
        char valBuf[32];
        if (step >= 1.f) {
            std::snprintf(valBuf, sizeof(valBuf), "%d", (int)(*value + 0.5f));
        } else {
            std::snprintf(valBuf, sizeof(valBuf), "%.2f", *value);
        }
        float vw = ui.textWidth(valBuf, 1.4f);
        ui.text(valBuf, r.x + r.w - vw,
                r.y + (r.h - ui.textHeight(1.4f)) * 0.5f,
                1.4f, rgba(220, 220, 220, 255));
    }

    return changed;
}

bool toggleWidget(UiContext& ui,
                  Rect r,
                  bool* value,
                  const std::string& label,
                  std::function<void(bool)> onChanged)
{
    if (!value) return false;

    int idx = ui.pushInteractiveRect(r, nullptr);
    bool pressed = ui.isInteractivePressed(idx);
    bool changed = false;

    // Триггер по release (через pendingTaps) — используем button().
    // Но нам нужен кастомный вид, поэтому ловим клик через isInteractivePressed
    // + pointerInside.
    if (ui.hasActivePointer() && ui.pointerInside(r)) {
        // не трогаем — тап произойдёт при release
    }

    UiColor bg = *value ? rgba(80, 160, 80, 220) : rgba(80, 80, 80, 220);
    if (pressed) bg = rgba(180, 180, 180, 255);

    ui.rect(r.x, r.y, r.w, r.h, bg);
    ui.rectOutline(r.x, r.y, r.w, r.h, 2.f, COL_BLACK);

    if (!label.empty()) {
        ui.text(label, r.x + 12.f, r.y + (r.h - ui.textHeight(1.6f)) * 0.5f,
                1.6f, COL_WHITE);
    }

    const char* stateStr = *value ? "ON" : "OFF";
    float sw = ui.textWidth(stateStr, 1.6f);
    ui.text(stateStr, r.x + r.w - sw - 12.f,
            r.y + (r.h - ui.textHeight(1.6f)) * 0.5f, 1.6f, COL_WHITE);

    (void)changed;
    (void)onChanged;
    return false;
}

bool cycleWidget(UiContext& ui,
                 Rect r,
                 const std::string& label,
                 const char* const* options,
                 u32 count,
                 u32* index,
                 std::function<void(u32)> onChanged)
{
    if (!index || !options || count == 0) return false;
    if (*index >= count) *index = 0;

    int idx = ui.pushInteractiveRect(r, nullptr);
    bool pressed = ui.isInteractivePressed(idx);

    UiColor bg = pressed ? rgba(180, 180, 180, 255) : rgba(60, 60, 80, 220);
    ui.rect(r.x, r.y, r.w, r.h, bg);
    ui.rectOutline(r.x, r.y, r.w, r.h, 2.f, COL_BLACK);

    if (!label.empty()) {
        ui.text(label, r.x + 12.f, r.y + (r.h - ui.textHeight(1.6f)) * 0.5f,
                1.6f, COL_WHITE);
    }

    const char* cur = options[*index];
    float cw = ui.textWidth(cur, 1.6f);
    ui.text(cur, r.x + r.w - cw - 12.f,
            r.y + (r.h - ui.textHeight(1.6f)) * 0.5f, 1.6f,
            rgba(255, 240, 160, 255));

    (void)onChanged;
    return false;
}

} // namespace ui
