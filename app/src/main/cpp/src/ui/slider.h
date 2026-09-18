/**
 * @file slider.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#pragma once
#include "ui_context.h"
#include <functional>
#include <string>

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

/// Переключатель и выбор из списка ТОЛЬКО РИСУЮТ.
///
/// Интерактивную область заводит место вызова: оно одно знает, что
/// делать по тапу, и оно же хранит значение. Виджету нужен лишь номер
/// этой области — чтобы показать нажатие.
///
/// Раньше оба заводили СВОЮ область поверх чужой, с пустым
/// обработчиком. Тап разбирается с конца списка, побеждает
/// зарегистрированный последним — и обработчик места вызова не
/// срабатывал никогда. Девять переключателей настроек, включая язык,
/// автосохранение и снятие ограничения кадров, рисовались, нажимались
/// и не делали ничего.
void toggleWidget(UiContext& ui,
                  Rect r,
                  int interactiveIdx,
                  const bool* value,
                  const std::string& label);

void cycleWidget(UiContext& ui,
                 Rect r,
                 int interactiveIdx,
                 const std::string& label,
                 const char* const* options,
                 u32 count,
                 const u32* index);

} // namespace ui
