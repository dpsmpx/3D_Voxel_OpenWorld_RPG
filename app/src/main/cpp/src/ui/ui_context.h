/**
 * @file ui_context.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "../config/localization.h"
#include "ui_types.h"
#include "ui_renderer.h"
#include <functional>
#include <vector>
#include <string>
#include <utility>

namespace ui {

constexpr UiColor COL_WHITE  = rgba(255,255,255,255);
constexpr UiColor COL_BLACK  = rgba(  0,  0,  0,255);
constexpr UiColor COL_RED    = rgba(220, 50, 50,255);
constexpr UiColor COL_GREEN  = rgba( 80,200, 80,255);
constexpr UiColor COL_BLUE   = rgba( 60,120,220,255);
constexpr UiColor COL_PURPLE = rgba(160, 80,220,255);
constexpr UiColor COL_YELLOW = rgba(240,220, 60,255);
constexpr UiColor COL_GRAY   = rgba(140,140,140,255);

class UiContext {
public:
    void init(UiRenderer* r, i32 screenW, i32 screenH);
    void setScreen(i32 w, i32 h) { screenW_ = w; screenH_ = h; }

    void beginFrame();
    void endFrame();

    /// ---- Примитивы ----
    void rect(float x, float y, float w, float h, UiColor c);
    void rectOutline(float x, float y, float w, float h, float thickness, UiColor c);
    /// Прямоугольник с картинкой снаружи (см. UiRenderer::setImage).
    /// Без установленной картинки рисует заливку цветом c.
    void imageQuad(float x, float y, float w, float h, UiColor c);
    /// Есть ли что показывать в imageQuad.
    bool hasImage() const;
    /// Картинка для imageQuad; владеет ею вызывающий.
    void setImage(VkImageView view, VkSampler sampler);
    void text(const std::string& s, float x, float y, float scale, UiColor c);

    /// Куда складывать всё, что вышло на экран буквами.
    ///
    /// Спросить «что написано на экране» иначе нечем: интерфейс
    /// собирается в вершины, и по ним строку обратно не прочесть.
    /// Проверка языка, спрашивавшая вместо экрана словарь, ровно на
    /// этом и промахнулась: перевод существовал, а место показа
    /// звало имя напрямую и об этом никто не знал.
    ///
    /// В игре указатель нулевой, и цена — одна проверка на строку.
    void setTextSink(std::vector<std::string>* sink) { textSink_ = sink; }
    /// Круг и кольцо в пикселях экрана. Экранные кнопки и джойстик
    /// круглые: из прямоугольников они выглядят как лесенка.
    void circle(float cx, float cy, float r, UiColor c, int segments = 24);
    void ring(float cx, float cy, float rInner, float rOuter, UiColor c,
              int segments = 24);
    float textWidth(const std::string& s, float scale) const;
    /// Текст с переносом по словам. Возвращает занятую высоту.
    ///
    /// Реплика NPC рисовалась одной строкой и уходила за панель.
    /// Перенос по словам, а не по символам: рвать слово посреди —
    /// хуже, чем перенести его целиком.
    float textWrapped(const std::string& s, float x, float y,
                      float maxWidth, float scale, UiColor c);
    /// Сколько места займёт такой текст, ничего не рисуя.
    float wrappedHeight(const std::string& s, float maxWidth, float scale) const;
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
    static bool sameRect(const Rect& a, const Rect& b);

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

    /// Начатое нажатие живёт ОТДЕЛЬНО от списка прямоугольников.
    ///
    /// Интерфейс здесь immediate-mode: interactives_ собирается заново
    /// на каждом кадре, а beginFrame() очищает его. Нажатие же длится
    /// сотню миллисекунд, то есть пять-семь кадров. Пометка «этот
    /// прямоугольник держит палец номер N» жила внутри списка — и
    /// стиралась первой же перерисовкой. К моменту отпускания искать
    /// было уже некого, и обработчик не вызывался НИКОГДА, кроме
    /// случая, когда палец успевал подняться в том же кадре.
    ///
    /// Поэтому при нажатии запоминаем сам прямоугольник и его
    /// обработчик здесь: перерисовка их не трогает.
    struct Capture {
        bool  active = false;
        i32   touchId = -1;
        Rect  rect{0.f, 0.f, 0.f, 0.f};
        std::function<void()> onTap;
        bool  inside = false;
    };

    UiRenderer* r_ = nullptr;
    std::vector<std::string>* textSink_ = nullptr;
    i32 screenW_ = 1080;
    i32 screenH_ = 1920;

    std::vector<Interactive> interactives_;
    std::vector<PendingTap>  pendingTaps_;
    Capture                  capture_;

    /// Активный тач — тот, который начал первое касание в этом кадре
    /// и ещё не завершился. Используется для слайдеров и drag.
    bool  pointerActive_ = false;
    i32   activePointerId_ = -1;
    float pointerX_ = 0.f;
    float pointerY_ = 0.f;
};

} // namespace ui
