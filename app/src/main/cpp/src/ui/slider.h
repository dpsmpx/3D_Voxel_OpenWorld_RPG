/**
 * @file slider.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#pragma once
#include "ui_context.h"
#include <functional>

namespace ui {

/// Слайдер: горизонтальный трек с бегунком.
/// Использует UiContext::pointerX() для корректного drag.
///
/// Возвращает true, если значение изменилось в этом кадре.
bool sliderWidget(UiContext& ui,
                  Rect r,
                  float* value,
                  float minV,
                  float maxV,
                  const std::string& label,
                  float step = 0.f,
                  std::function<void(float)> onChanged = nullptr);

bool toggleWidget(UiContext& ui,
                  Rect r,
                  bool* value,
                  const std::string& label,
                  std::function<void(bool)> onChanged = nullptr);

bool cycleWidget(UiContext& ui,
                 Rect r,
                 const std::string& label,
                 const char* const* options,
                 u32 count,
                 u32* index,
                 std::function<void(u32)> onChanged = nullptr);

} // namespace ui
