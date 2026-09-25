/**
 * @file slider.cpp
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#include "slider.h"
#include "../config/localization.h"
#include "../core/log.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>

namespace ui {

namespace {
/// Подпись слева и значение справа — в одну строку, пока они не
/// налезают друг на друга; иначе подпись сверху, значение снизу
/// справа. Узкая колонка настроек укладывала «ИНВЕРСИЯ ПО X» поверх
/// «ВЫКЛ».
void labelAndValue(UiContext& ui, Rect r, const std::string& label,
                   const char* value, UiColor valueColor) {
    const float pad = 12.f;
    const float th = ui.textHeight(1.6f);
    const float lw = label.empty() ? 0.f : ui.textWidth(label, 1.6f);
    const float vw = ui.textWidth(value, 1.6f);
    const bool stacked = lw + vw + pad * 3.f > r.w;
    if (stacked) {
        // Подпись шире строки — на ступень мельче.
        const float ls = lw + pad * 2.f > r.w ? 1.3f : 1.6f;
        const float gapY = (r.h - th * 2.f) / 3.f;
        if (!label.empty()) ui.text(label, r.x + pad, r.y + gapY, ls, COL_WHITE);
        ui.text(value, r.x + r.w - vw - pad, r.y + gapY * 2.f + th, 1.6f, valueColor);
    } else {
        const float y = r.y + (r.h - th) * 0.5f;
        if (!label.empty()) ui.text(label, r.x + pad, y, 1.6f, COL_WHITE);
        ui.text(value, r.x + r.w - vw - pad, y, 1.6f, valueColor);
    }
}
} // namespace

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

    // Подпись слева от дорожки — пока та остаётся дорожкой. Узкая
    // колонка настроек оставляла ей двадцать точек, и подпись,
    // бегунок и число ложились друг на друга. Тогда подпись и число
    // встают строкой над дорожкой, а дорожка берёт всю ширину.
    char valBuf[32];
    if (step >= 1.f) std::snprintf(valBuf, sizeof(valBuf), "%d", (int)(*value + 0.5f));
    else             std::snprintf(valBuf, sizeof(valBuf), "%.2f", *value);
    const float valW = ui.textWidth(valBuf, 1.4f);

    const float labelW = label.empty() ? 0.f : ui.textWidth(label, 1.6f);
    const float gap = 12.f;
    const float trackH = 12.f;
    float trackX = r.x + labelW + (label.empty() ? 0.f : gap);
    float trackW = r.w - labelW - (label.empty() ? 0.f : gap) - valW - gap;
    float trackY = r.y + (r.h - trackH) * 0.5f;
    const bool stacked = !label.empty() && trackW < r.w * 0.40f;
    // Подпись шире самой строки — на ступень мельче: иначе она уходит
    // в соседнюю колонку.
    const float labelScale = labelW > r.w ? 1.3f : 1.6f;
    if (stacked) {
        trackX = r.x;
        trackW = r.w - valW - gap;
        trackY = r.y + r.h - trackH - 12.f;
    }
    if (trackW < 20.f) trackW = 20.f;

    // Касание ловит вся часть строки от начала дорожки до правого края
    // (при подписи сверху — вся строка): палец, уехавший на число за
    // концом дорожки, продолжает тянуть бегунок, а не теряет его.
    const Rect hit = stacked ? r
                             : Rect{ trackX, r.y, r.x + r.w - trackX, r.h };
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

    // Значение — справа: в строку с подписью, если та встала над
    // дорожкой, иначе в конце дорожки. Число считалось заново уже
    // ПОСЛЕ перетаскивания — берём то же, что показывает бегунок.
    if (step >= 1.f) std::snprintf(valBuf, sizeof(valBuf), "%d", (int)(*value + 0.5f));
    else             std::snprintf(valBuf, sizeof(valBuf), "%.2f", *value);
    const float lineY = stacked ? r.y + 4.f
                                : r.y + (r.h - ui.textHeight(labelScale)) * 0.5f;
    if (!label.empty()) ui.text(label, r.x, lineY, labelScale, COL_WHITE);
    ui.text(valBuf, r.x + r.w - ui.textWidth(valBuf, 1.4f),
            trackY + (trackH - ui.textHeight(1.4f)) * 0.5f,
            1.4f, rgba(220, 220, 220, 255));

    return changed;
}

void toggleWidget(UiContext& ui,
                  Rect r,
                  int interactiveIdx,
                  const bool* value,
                  const std::string& label)
{
    if (!value) return;

    const bool pressed = ui.isInteractivePressed(interactiveIdx);

    UiColor bg = *value ? rgba(80, 160, 80, 220) : rgba(80, 80, 80, 220);
    if (pressed) bg = rgba(180, 180, 180, 255);

    ui.rect(r.x, r.y, r.w, r.h, bg);
    ui.rectOutline(r.x, r.y, r.w, r.h, 2.f, COL_BLACK);

    // Ключи On и Off в таблице строк были с самого начала и не
    // звались ни разу: тумблеры настроек писали «ON» и «OFF»
    // литералом, и русский экран настроек был наполовину
    // английским.
    const char* stateStr = config::T(*value ? config::StrKey::On
                                            : config::StrKey::Off);
    labelAndValue(ui, r, label, stateStr, COL_WHITE);
}

void cycleWidget(UiContext& ui,
                 Rect r,
                 int interactiveIdx,
                 const std::string& label,
                 const char* const* options,
                 u32 count,
                 const u32* index)
{
    if (!index || !options || count == 0) return;
    const u32 cur = *index < count ? *index : 0;

    const bool pressed = ui.isInteractivePressed(interactiveIdx);

    UiColor bg = pressed ? rgba(180, 180, 180, 255) : rgba(60, 60, 80, 220);
    ui.rect(r.x, r.y, r.w, r.h, bg);
    ui.rectOutline(r.x, r.y, r.w, r.h, 2.f, COL_BLACK);

    labelAndValue(ui, r, label, options[cur], rgba(255, 240, 160, 255));
}

} // namespace ui
