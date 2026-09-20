/**
 * @file hud_layout.h
 * @brief Раскладка HUD — единственный источник геометрии интерфейса.
 */
#pragma once
#include "../core/types.h"
#include "ui_types.h"
#include "ui_theme.h"
#include <algorithm>

namespace ui {

/// Безопасная зона: отступы от краёв экрана в точках.
///
/// Сейчас все нули: манифест не просит `layoutInDisplayCutoutMode`, и
/// система сама держит окно вне выреза — наблюдаемые 2306 точек при
/// панели 2400 это и есть её отступ. Поле заведено затем, что как
/// только кто-нибудь включит SHORT_EDGES ради полной панели, все
/// привязанные к краю элементы поедут разом, и чинить придётся в
/// четырнадцати местах вместо одного.
struct SafeInsets {
    f32 left = 0.f, top = 0.f, right = 0.f, bottom = 0.f;
};

/// К какому углу экрана привязана круглая кнопка.
enum class PadAnchor : u8 { BottomLeft, BottomRight, TopRight };

/// Круглая кнопка по умолчанию: отступ от угла и диаметр, всё в dp.
///
/// Порядок обязан совпадать с config::ButtonSlot — по нему
/// раскладываются сохранённые пользовательские сдвиги.
struct PadDef {
    PadAnchor   anchor;
    f32         dx, dy;
    f32         diameter;
    const char* label;
};

/// Места подобраны счётом, а не на глаз: проверка перебирает шесть
/// разрешений и требует ноль пересечений с HUD и между собой.
/// Диаметры — не меньше 48 dp, ниже палец промахивается; прежние
/// ITM и CAM были 44 dp.
inline constexpr PadDef PAD_BUTTONS[] = {
    { PadAnchor::BottomRight, 150.f, 150.f, 84.f, "ATK" },
    { PadAnchor::BottomRight, 140.f, 236.f, 52.f, "FIN" },
    { PadAnchor::BottomRight, 238.f, 104.f, 60.f, "JMP" },
    // Здесь была RUN, 64 dp на левой половине. Бег переехал на
    // двойное нажатие джойстика, и место освободилось — левая
    // половина экрана и так тесная.
    { PadAnchor::BottomRight, 252.f, 192.f, 52.f, "DIG" },
    { PadAnchor::BottomLeft,   56.f, 226.f, 52.f, "PUT" },
    { PadAnchor::BottomLeft,  150.f, 196.f, 52.f, "USE" },
    { PadAnchor::BottomLeft,  152.f, 116.f, 52.f, "ITM" },
    { PadAnchor::TopRight,     42.f, 212.f, 52.f, "CAM" },
    // Рывок — на левой половине, у большого пальца, которым и
    // держат направление: рвутся ТУДА, КУДА ИДУТ, и тянуться за
    // этим через весь экран нечем.
    { PadAnchor::BottomLeft,  246.f, 132.f, 56.f, "DSH" },
};
constexpr u32 PAD_BUTTON_COUNT =
    (u32)(sizeof(PAD_BUTTONS) / sizeof(PAD_BUTTONS[0]));

/// Геометрия HUD.
///
/// И отрисовка, и проверка касания берут прямоугольники ОТСЮДА.
///
/// Раньше модель жила здесь, а отрисовка HUD считала свои числа
/// заново и без общего масштаба. На 1280x720 расхождение
/// доходило до 191 точки, четыре кнопки реально накрывали HUD, а
/// проверка наложений рапортовала «ни одного»: она сверяла модель с
/// моделью. При высоте 1080 расхождение было ровно ноль — на таком
/// телефоне игру и смотрели.
///
/// Все размеры заданы в dp и переводятся через настоящую плотность
/// экрана (ui_theme.h, раздел 1). Прежний hudScale = screenH / 1080
/// считал от числа пикселей и давал на двух телефонах одного размера
/// кнопки, различающиеся в полтора раза.
class HudLayout {
public:
    HudLayout() = default;
    HudLayout(f32 screenW, f32 screenH, theme::Metrics m, SafeInsets si,
              bool mirrored = false)
        : w_(screenW), h_(screenH), m_(m), si_(si), mirror_(mirrored) {}

    /// Раскладка для левши: зеркалится ВЕСЬ экран, а не одни кнопки.
    ///
    /// Раньше отражалось только управление, а столбец навигации
    /// оставался справа — и кнопки левши приезжали ровно
    /// под него. На 1280x720 «PUT» попадала под навигацию. Ловить это
    /// подбором координат бессмысленно: если зеркалить всё, зеркальный
    /// случай устроен точно так же, как обычный, и проверять его
    /// достаточно один раз.
    bool mirrored() const { return mirror_; }

    f32 width()  const { return w_; }
    f32 height() const { return h_; }
    const theme::Metrics& metrics() const { return m_; }

    f32 dp(f32 v) const { return m_.dp(v); }

private:
    /// Отражение по горизонтали. Применяется в самом конце, ко всему
    /// одинаково, — поэтому зеркальная раскладка не может разойтись с
    /// обычной.
    Rect flip(Rect r) const {
        if (mirror_) r.x = w_ - (r.x + r.w);
        return r;
    }
    f32 flipX(f32 x) const { return mirror_ ? w_ - x : x; }

public:

    // ---- края рабочей области ----
    f32 left()   const { return si_.left; }
    f32 top()    const { return si_.top; }
    f32 right()  const { return w_ - si_.right; }
    f32 bottom() const { return h_ - si_.bottom; }

    // ---- полоса опыта: во всю рабочую ширину, вверху ----
    Rect xpBar() const {
        return flip({ left(), top() + dp(theme::SPACE_XS_DP),
                      right() - left(), dp(XP_BAR_H_DP) });
    }

    // ---- плашка подгрузки: сверху по центру ----
    //
    // Раньше на время генерации мира весь экран затемнялся плотной
    // заливкой. Мир при этом уже нарисован и уже играбелен: чанки
    // подгружаются вокруг игрока, ближние готовы первыми. Гасить всё
    // ради полосы прогресса — значит прятать от игрока ровно то, ради
    // чего он ждёт.
    //
    // Верх по центру свободен: слева полосы ресурсов, справа столбец
    // навигации, а между ними ничего нет.
    Rect loadingPanel() const {
        const f32 wdt = dp(LOADING_W_DP);
        const f32 hgt = dp(LOADING_H_DP);
        const Rect xb = xpBar();
        // По центру рабочей области; зеркалить не нужно — центр
        // симметричен, и flip оставил бы плашку на месте.
        return { left() + (right() - left() - wdt) * 0.5f,
                 xb.y + xb.h + dp(theme::SPACE_M_DP), wdt, hgt };
    }

    // ---- полосы ресурсов: слева под опытом ----
    static constexpr u32 RES_BARS = 3;   ///< здоровье, мана, выносливость
    Rect resourceBar(u32 i) const {
        const f32 h = dp(RES_BAR_H_DP), gap = dp(theme::SPACE_XS_DP);
        const Rect xb = xpBar();
        return flip({ left() + dp(theme::SPACE_L_DP),
                      xb.y + xb.h + dp(theme::SPACE_M_DP) + (f32)i * (h + gap),
                      dp(RES_BAR_W_DP), h });
    }

    // ---- кошелёк: сразу под полосами ----
    Rect goldLine() const {
        const Rect last = resourceBar(RES_BARS - 1);
        return { last.x, last.y + last.h + dp(theme::SPACE_S_DP),
                 dp(RES_BAR_W_DP), dp(GOLD_LINE_H_DP) };
    }

    // ---- воздух: показывается только под водой ----
    Rect airBar() const {
        const Rect last = resourceBar(RES_BARS - 1);
        return { last.x, last.y + last.h + dp(22.f),
                 dp(RES_BAR_W_DP), dp(RES_BAR_H_DP) * 0.6f };
    }

    /// Служебные показания: частота кадров и координаты.
    ///
    /// Стояли по постоянным 180 и 204 точкам от верха экрана — а
    /// полосы ресурсов считаются от плотности: на 420 dpi они кончают
    /// ся на 194-й точке, на 480 — на 222-й. Счётчик кадров лежал
    /// прямо на полосе выносливости, а на плотном экране накрывал все
    /// три полосы разом.
    ///
    /// Левый столбец занят целиком — от полос до круглых кнопок, — и
    /// двух строк в нём нет ни на одном из размеров. Правый край
    /// зажат между столбцом навигации и кнопкой обзора. Зато свободна
    /// верхняя середина: слева полосы, справа навигация, а между ними
    /// одна плашка подгрузки, и та только на время генерации.
    /// Показания уходят под неё — ниже прежнего и ничего не закрывая.
    static constexpr u32 DEBUG_LINES = 2;   ///< кадры, координаты
    Rect debugLine(u32 i) const {
        const Rect lp = loadingPanel();
        const f32 h = dp(DEBUG_LINE_H_DP);
        // Зеркалить не нужно: плашка по центру, а центр симметричен.
        return { lp.x, lp.y + lp.h + dp(theme::SPACE_S_DP) + (f32)i * h,
                 lp.w, h };
    }

    /// Нижняя граница служебных строк. Важные уведомления всплывают
    /// в той же середине и обязаны начинаться ниже.
    f32 debugBottom() const {
        const Rect last = debugLine(DEBUG_LINES - 1);
        return last.y + last.h;
    }

    // ---- две кнопки навигации справа вверху ----
    //
    // Их было семь. При нормальном размере касания столбец из семи
    // занял бы 78 % высоты экрана — раскладка была несовместима с
    // размером пальца. Остальное живёт в паузе, куда ведёт первая же
    // кнопка.
    Rect navButton(u32 i) const {
        const f32 s = dp(theme::TOUCH_REGULAR_DP);
        return flip({ right() - dp(theme::SPACE_L_DP) - s,
                      top() + dp(theme::SPACE_L_DP)
                          + (f32)i * (s + dp(theme::TOUCH_GAP_DP)),
                      s, s });
    }
    static constexpr u32 NAV_COUNT = 2;   ///< пауза, инвентарь

    /// Нижняя граница столбца навигации: ниже правый край свободен.
    f32 navBottom() const {
        const Rect r = navButton(NAV_COUNT - 1);
        return r.y + r.h;
    }

    // ---- пояс предметов: по центру нижнего края ----
    //
    // Число ВИДИМЫХ ячеек подбирается под ширину. Девять ячеек по
    // 56 dp занимают 60 % ширины рабочего телефона и 86 % бюджетного
    // 1280x720 — там они налезают на круглые кнопки. Уменьшать
    // ячейку нельзя: ниже 48 dp палец промахивается. Значит на узком
    // экране ячеек показывается меньше.
    //
    // В ДАННЫХ их по-прежнему девять: число лежит в формате
    // сохранения и меняться не должно. Видимость и ёмкость — разные
    // вещи, и доступ к остальным ячейкам даёт инвентарь.
    f32 hotbarSlotSize() const { return dp(theme::TOUCH_REGULAR_DP); }
    f32 hotbarGap()      const { return dp(HOTBAR_GAP_DP); }

    /// Сколько ячеек помещается между зонами управления.
    ///
    /// Пояс стоит по центру, поэтому ограничение симметрично: он не
    /// должен заходить за круглые кнопки ни по одну сторону, ни по
    /// другую. Сколько ячеек помещается — прямое следствие того, что
    /// ячейку нельзя сделать мельче 48 dp, а углы заняты управлением.
    u32 hotbarVisibleSlots() const {
        const f32 s = hotbarSlotSize(), g = hotbarGap();
        const f32 gap = dp(theme::TOUCH_GAP_DP);

        // Сторону определяем по фактическому положению, а не по
        // якорю: при зеркальной раскладке «правая» кнопка стоит
        // слева, и якорь сказал бы неправду.
        const f32 mid = w_ * 0.5f;
        const f32 rowTop = bottom() - dp(theme::SPACE_M_DP) - s;
        const f32 rowBot = rowTop + s;

        f32 rlim = w_, llim = 0.f;
        auto consider = [&](f32 cx, f32 cL, f32 cR, f32 top, f32 bot) {
            // Мешает только то, что стоит на высоте пояса.
            if (bot <= rowTop || top >= rowBot) return;
            if (cx > mid) { if (cL < rlim) rlim = cL; }
            else          { if (cR > llim) llim = cR; }
        };

        for (u32 i = 0; i < PAD_BUTTON_COUNT; ++i) {
            const PadCircle c = padButton(i);
            consider(c.cx, c.cx - c.r, c.cx + c.r, c.cy - c.r, c.cy + c.r);
        }
        rlim -= gap;
        llim += gap;

        // Симметрично относительно центра: узкая сторона и решает.
        const f32 half = (rlim - w_ * 0.5f) < (w_ * 0.5f - llim)
                       ? (rlim - w_ * 0.5f) : (w_ * 0.5f - llim);
        const f32 avail = half * 2.f;
        if (avail <= s) return HOTBAR_MIN_VISIBLE;
        f32 n = (avail + g) / (s + g);
        u32 k = n <= 0.f ? 0u : (u32)n;
        if (k > HOTBAR_SLOTS) k = HOTBAR_SLOTS;
        if (k < HOTBAR_MIN_VISIBLE) k = HOTBAR_MIN_VISIBLE;
        return k;
    }

    Rect hotbar() const {
        const f32 s = hotbarSlotSize(), g = hotbarGap();
        const f32 total = (f32)hotbarVisibleSlots() * (s + g) - g;
        return { (w_ - total) * 0.5f, bottom() - dp(theme::SPACE_M_DP) - s,
                 total, s };
    }

    Rect hotbarSlot(u32 i) const {
        const Rect hb = hotbar();
        const f32 s = hotbarSlotSize();
        return { hb.x + (f32)i * (s + hotbarGap()), hb.y, s, s };
    }

    // ---- подсказка взаимодействия: над поясом, по центру ----
    //
    // Её не было вовсе, и это стоило игроку трёх экранов: близость
    // станка и алтаря игра считает каждый кадр, а показать было нечем.
    Rect interactPrompt() const {
        const f32 wdt = dp(PROMPT_W_DP), hgt = dp(theme::TOUCH_PRIMARY_DP);
        return { (w_ - wdt) * 0.5f,
                 hotbar().y - dp(theme::SPACE_L_DP) - hgt, wdt, hgt };
    }

    // ============================================================
    // Меню на весь экран: пауза и её разделы
    // ============================================================
    //
    // Пауза была столбцом из восьми кнопок, каждая своего цвета, без
    // группировки. В альбомной ориентации столбец — худшая из форм:
    // по вертикали места меньше всего, а по горизонтали оно пустует.
    // Поэтому сетка.

    /// Область, отведённая содержимому полноэкранного меню.
    Rect menuArea() const {
        const f32 pad = dp(theme::SPACE_XL_DP);
        return { left() + pad, top() + dp(MENU_TITLE_DP) + pad,
                 (right() - left()) - pad * 2.f,
                 (bottom() - top()) - dp(MENU_TITLE_DP) - pad * 2.f };
    }

    /// Заголовок меню — над областью содержимого.
    Rect menuTitle() const {
        const f32 pad = dp(theme::SPACE_XL_DP);
        return { left() + pad, top() + pad,
                 (right() - left()) - pad * 2.f, dp(MENU_TITLE_DP) };
    }

    /// Ячейка сетки меню. Ряды считаются сверху, колонки слева.
    ///
    /// Высота ячейки не опускается ниже обычной цели касания: если
    /// рядов столько, что не помещаются, виновата не ячейка, а
    /// количество пунктов — их и надо группировать.
    Rect menuCell(u32 col, u32 row, u32 cols, u32 rows) const {
        const Rect a = menuArea();
        const f32 gap = dp(theme::SPACE_M_DP);
        const f32 cw = (a.w - gap * (f32)(cols - 1)) / (f32)cols;
        f32 ch = (a.h - gap * (f32)(rows - 1)) / (f32)rows;
        const f32 minH = dp(theme::TOUCH_REGULAR_DP);
        if (ch < minH) ch = minH;
        return { a.x + (f32)col * (cw + gap),
                 a.y + (f32)row * (ch + gap), cw, ch };
    }

    // ---- сетка ячеек фиксированного размера ----
    //
    // Ячейка не может быть мельче цели касания, поэтому под данное
    // количество подбирается не размер, а число столбцов. Сумка из
    // 27 ячеек в девять столбцов занимает 552 dp — на узком экране их
    // просто меньше, а рядов больше.
    struct CellGrid {
        Rect area;
        f32  cell = 0.f, gap = 0.f;
        u32  cols = 1, rows = 1;

        Rect at(u32 i) const {
            const u32 c = cols ? (i % cols) : 0;
            const u32 r = cols ? (i / cols) : 0;
            return { area.x + (f32)c * (cell + gap),
                     area.y + (f32)r * (cell + gap), cell, cell };
        }
        /// Сколько места сетка занимает на самом деле.
        Rect bounds() const {
            return { area.x, area.y,
                     (f32)cols * (cell + gap) - gap,
                     (f32)rows * (cell + gap) - gap };
        }
    };

    CellGrid cellGrid(Rect area, u32 count) const {
        CellGrid g;
        g.area = area;
        g.cell = dp(theme::TOUCH_REGULAR_DP);
        g.gap  = dp(theme::SPACE_S_DP);
        if (count == 0) { g.cols = g.rows = 0; return g; }
        f32 fit = (area.w + g.gap) / (g.cell + g.gap);
        g.cols = fit < 1.f ? 1u : (u32)fit;
        if (g.cols > count) g.cols = count;
        g.rows = (count + g.cols - 1) / g.cols;
        return g;
    }

    // ---- журнал заданий: список слева, подробности справа ----
    Rect questList() const {
        const Rect a = menuArea();
        return { a.x, a.y, a.w * (1.f - QUEST_DETAILS_FRAC)
                            - dp(theme::SPACE_L_DP), a.h };
    }
    Rect questDetails() const {
        const Rect a = menuArea();
        const f32 wdt = a.w * QUEST_DETAILS_FRAC;
        return { a.x + a.w - wdt, a.y, wdt, a.h };
    }
    /// Строка списка. Высота — обычная цель касания плюс место под
    /// полосу прогресса.
    Rect questRow(u32 i) const {
        const Rect l = questList();
        const f32 h = dp(theme::TOUCH_REGULAR_DP);
        const f32 g = dp(theme::SPACE_S_DP);
        return { l.x, l.y + (f32)i * (h + g), l.w, h };
    }
    u32 questRowsVisible() const {
        const Rect l = questList();
        const f32 h = dp(theme::TOUCH_REGULAR_DP) + dp(theme::SPACE_S_DP);
        const f32 n = h > 0.f ? l.h / h : 0.f;
        return n < 1.f ? 1u : (u32)n;
    }

    /// Строка текущей цели на HUD — под полосой опыта, слева.
    ///
    /// Её не было: чтобы узнать, что делать, приходилось открывать
    /// журнал, а журнал показывал только ЧИСЛО активных заданий.
    Rect questTracker() const {
        const Rect last = resourceBar(RES_BARS - 1);
        return { last.x, last.y + last.h + dp(theme::SPACE_XL_DP),
                 dp(TRACKER_W_DP), dp(TRACKER_H_DP) };
    }

    static constexpr f32 QUEST_DETAILS_FRAC = 0.40f;
    static constexpr f32 TRACKER_W_DP = 220.f;
    static constexpr f32 TRACKER_H_DP =  40.f;

    // ---- общее для полноэкранных экранов ----
    //
    // Кнопка закрытия была написана пятью одинаковыми копиями
    // `Rect close{ screenW - 90, 74, 80, 50 }` — 20 dp по высоте при
    // норме 48. Один компонент — одно место, где он задан.
    Rect closeButton() const {
        const Rect t = menuTitle();
        const f32 sz = dp(theme::TOUCH_REGULAR_DP);
        return { t.x + t.w - sz, t.y + (t.h - sz) * 0.5f, sz, sz };
    }

    /// Две панели: список слева, подробности справа.
    Rect paneLeft() const {
        const Rect a = menuArea();
        return { a.x, a.y, a.w * (1.f - PANE_RIGHT_FRAC)
                            - dp(theme::SPACE_L_DP), a.h };
    }
    Rect paneRight() const {
        const Rect a = menuArea();
        const f32 wdt = a.w * PANE_RIGHT_FRAC;
        return { a.x + a.w - wdt, a.y, wdt, a.h };
    }

    /// Строка списка в левой панели.
    Rect paneRow(u32 i) const {
        const Rect l = paneLeft();
        const f32 h = dp(theme::TOUCH_REGULAR_DP);
        const f32 g = dp(theme::SPACE_S_DP);
        return { l.x, l.y + (f32)i * (h + g), l.w, h };
    }
    u32 paneRowsVisible() const {
        const Rect l = paneLeft();
        const f32 h = dp(theme::TOUCH_REGULAR_DP) + dp(theme::SPACE_S_DP);
        const f32 n = h > 0.f ? l.h / h : 0.f;
        return n < 1.f ? 1u : (u32)n;
    }

    /// Главное действие экрана — внизу правой панели.
    Rect primaryAction() const {
        const Rect r = paneRight();
        const f32 pad = dp(theme::PANEL_PAD_DP);
        const f32 h = dp(theme::TOUCH_PRIMARY_DP);
        return { r.x + pad, r.y + r.h - pad - h, r.w - pad * 2.f, h };
    }

    static constexpr f32 PANE_RIGHT_FRAC = 0.38f;

    // ---- характеристики: строка и кнопки прибавить/убавить ----
    //
    // Кнопки были 50 точек — на рабочем телефоне это 20 dp при норме
    // 48, то есть 3.2 мм. Ровно тот же дефект, что и везде: размер
    // задавался в пикселях и не зависел от плотности.
    Rect attrRow(u32 i) const {
        const Rect a = menuArea();
        const f32 minH = dp(theme::TOUCH_REGULAR_DP);
        const f32 maxH = dp(ATTR_ROW_H_DP);

        // Высота подстраивается под область, а не задаётся числом:
        // четыре строки по 88 dp не помещались ни на один экран,
        // кроме планшета. Ниже цели касания строка не опускается —
        // в ней стоят кнопки. Если при обычном зазоре ряды всё же не
        // влезают, ужимается ЗАЗОР, а не строка: расстояние между
        // рядами можно потерять, нажимаемость — нет.
        f32 g = dp(theme::SPACE_M_DP);
        f32 h = (a.h - g * (f32)(ATTR_COUNT - 1)) / (f32)ATTR_COUNT;
        if (h < minH) {
            g = dp(theme::SPACE_XS_DP);
            h = (a.h - g * (f32)(ATTR_COUNT - 1)) / (f32)ATTR_COUNT;
        }
        if (h > maxH) h = maxH;
        if (h < minH) h = minH;

        const f32 wdt = a.w < dp(ATTR_ROW_MAX_W_DP) ? a.w : dp(ATTR_ROW_MAX_W_DP);
        return { a.x + (a.w - wdt) * 0.5f, a.y + (f32)i * (h + g), wdt, h };
    }

    /// i — номер строки, plus — прибавить (иначе убавить).
    Rect attrButton(u32 i, bool plus) const {
        const Rect r = attrRow(i);
        const f32 sz = dp(theme::TOUCH_REGULAR_DP);
        const f32 pad = dp(theme::SPACE_M_DP);
        const f32 gap = dp(theme::TOUCH_GAP_DP);
        const f32 px = r.x + r.w - pad - sz;
        return { plus ? px : px - sz - gap,
                 r.y + (r.h - sz) * 0.5f, sz, sz };
    }

    static constexpr u32 ATTR_COUNT        =   4;
    static constexpr f32 ATTR_ROW_H_DP     =  88.f;   ///< потолок
    static constexpr f32 ATTR_ROW_MAX_W_DP = 520.f;

    // ---- уведомления ----
    //
    // Важное по центру, рядовое снизу. Место — тоже признак: по
    // одному цвету отличить «новый уровень» от «предмет получен»
    // нельзя.
    Rect notice(u32 i, bool high, f32 textWidth) const {
        const f32 pad = dp(theme::SPACE_L_DP);
        const f32 hgt = dp(high ? NOTICE_HIGH_H_DP : NOTICE_H_DP);
        const f32 wdt = textWidth + pad * 2.f;
        const f32 gap = dp(theme::SPACE_S_DP);

        // Важное — в верхней трети, но НИЖЕ служебных строк. Доля от
        // высоты экрана и раскладка служебных строк считаются от
        // разного: на 1280x720 уведомление начиналось на 202-й точке,
        // а вторая служебная строка кончалась на 208-й — и накрывало
        // её собой.
        if (high) {
            const f32 y = std::max(h_ * NOTICE_HIGH_Y_FRAC,
                                   debugBottom() + dp(theme::SPACE_L_DP));
            return { (w_ - wdt) * 0.5f, y, wdt, hgt };
        }

        // Рядовые стопкой снизу вверх, над подсказкой и поясом.
        const f32 base = interactPrompt().y - dp(theme::SPACE_L_DP);
        return { (w_ - wdt) * 0.5f,
                 base - (f32)(i + 1) * (hgt + gap), wdt, hgt };
    }

    static constexpr f32 NOTICE_H_DP       = 44.f;
    static constexpr f32 NOTICE_HIGH_H_DP  = 64.f;
    static constexpr f32 NOTICE_HIGH_Y_FRAC = 0.28f;

    // ---- диалог: панель у нижнего края ----
    //
    // Разговор идёт В мире, поэтому панель не занимает экран целиком:
    // собеседника должно быть видно.
    Rect dialoguePanel() const {
        const f32 pad = dp(theme::SPACE_XL_DP);
        const f32 hgt = (bottom() - top()) * DIALOGUE_H_FRAC;
        return { left() + pad, bottom() - pad - hgt,
                 (right() - left()) - pad * 2.f, hgt };
    }

    /// Вариант ответа. y — где кончился текст реплики.
    Rect dialogueChoice(f32 y, u32 i) const {
        const Rect p = dialoguePanel();
        const f32 pad = dp(theme::PANEL_PAD_DP);
        const f32 h = dp(theme::TOUCH_REGULAR_DP);
        return { p.x + pad, y + (f32)i * (h + dp(theme::SPACE_S_DP)),
                 p.w - pad * 2.f, h };
    }

    static constexpr f32 DIALOGUE_H_FRAC = 0.45f;

    /// Окно подтверждения: по центру, не шире семидесяти процентов.
    Rect confirmPanel() const {
        const f32 wdt = (right() - left()) * CONFIRM_W_FRAC;
        const f32 hgt = dp(CONFIRM_H_DP);
        return { (w_ - wdt) * 0.5f, (h_ - hgt) * 0.5f, wdt, hgt };
    }

    /// Две кнопки внизу окна: отмена слева, подтверждение справа.
    Rect confirmButton(u32 i) const {
        const Rect p = confirmPanel();
        const f32 pad = dp(theme::PANEL_PAD_DP);
        const f32 gap = dp(theme::SPACE_M_DP);
        const f32 bh = dp(theme::TOUCH_PRIMARY_DP);
        const f32 bw = (p.w - pad * 2.f - gap) * 0.5f;
        return { p.x + pad + (f32)i * (bw + gap),
                 p.y + p.h - pad - bh, bw, bh };
    }

    // ============================================================
    // Инвентарь
    // ============================================================
    //
    // Слева сумка и экипировка, справа — сведения о выбранном
    // предмете. Игрок должен понимать, что это и можно ли с этим
    // что-то сделать, не гадая по цвету рамки.
    Rect invDetails() const {
        const Rect a = menuArea();
        const f32 wdt = a.w * INV_DETAILS_FRAC;
        return { a.x + a.w - wdt, a.y, wdt, a.h };
    }
    Rect invLeft() const {
        const Rect a = menuArea();
        return { a.x, a.y, a.w * (1.f - INV_DETAILS_FRAC)
                            - dp(theme::SPACE_L_DP), a.h };
    }

    /// Кнопка действия над предметом, внизу панели сведений.
    Rect invAction(u32 i, u32 count) const {
        const Rect d = invDetails();
        const f32 pad = dp(theme::PANEL_PAD_DP);
        const f32 gap = dp(theme::SPACE_S_DP);
        const f32 bh = dp(theme::TOUCH_REGULAR_DP);
        const f32 total = (f32)count * (bh + gap) - gap;
        return { d.x + pad, d.y + d.h - pad - total + (f32)i * (bh + gap),
                 d.w - pad * 2.f, bh };
    }

    static constexpr f32 INV_DETAILS_FRAC = 0.30f;

    // ---- свободный центр: сюда не залезает ничто ----
    Rect clearCenter() const {
        const f32 cw = w_ * theme::HUD_CLEAR_W_FRAC;
        const f32 ch = h_ * theme::HUD_CLEAR_H_FRAC;
        return { (w_ - cw) * 0.5f, (h_ - ch) * 0.5f, cw, ch };
    }

    // ============================================================
    // Круглые кнопки поверх мира
    // ============================================================
    //
    // Раньше они жили в input/touch_layout.h, отдельно от HUD, и
    // каждая сторона знала только свои числа. Наложения были видны
    // только на устройстве. Теперь вся экранная геометрия в одном
    // файле — иначе «согласовать два набора» превращается в работу,
    // которую делают глазами.
    struct PadCircle {
        PadAnchor   anchor;
        f32         cx, cy, r;
        const char* label;
    };

    PadCircle padButton(u32 i) const {
        const PadDef& d = PAD_BUTTONS[i < PAD_BUTTON_COUNT ? i : 0];
        const f32 dx = dp(d.dx), dy = dp(d.dy), r = dp(d.diameter) * 0.5f;
        f32 cx = 0.f, cy = 0.f;
        switch (d.anchor) {
            case PadAnchor::BottomLeft:  cx = left()  + dx; cy = bottom() - dy; break;
            case PadAnchor::BottomRight: cx = right() - dx; cy = bottom() - dy; break;
            case PadAnchor::TopRight:    cx = right() - dx; cy = top()    + dy; break;
        }
        return { d.anchor, flipX(cx), cy, r, d.label };
    }

    // ---- размеры в dp ----
    static constexpr f32 XP_BAR_H_DP  =   8.f;
    /// Плашка подгрузки: полоса прогресса с подписью, сверху по центру.
    static constexpr f32 LOADING_W_DP = 220.f;
    static constexpr f32 LOADING_H_DP =  44.f;
    static constexpr f32 RES_BAR_H_DP =  14.f;
    static constexpr f32 RES_BAR_W_DP = 160.f;
    static constexpr f32 GOLD_LINE_H_DP  = 12.f;
    static constexpr f32 DEBUG_LINE_H_DP = 12.f;
    static constexpr f32 HOTBAR_GAP_DP =  6.f;
    static constexpr f32 PROMPT_W_DP  = 260.f;
    /// Полоса заголовка вмещает цель касания: в ней стоит кнопка
    /// закрытия, и при 40 dp она вылезала за полосу.
    static constexpr f32 MENU_TITLE_DP   = theme::TOUCH_REGULAR_DP;
    static constexpr f32 CONFIRM_W_FRAC  = 0.70f;
    static constexpr f32 CONFIRM_H_DP    = 220.f;
    static constexpr u32 HOTBAR_SLOTS =   9;   ///< в данных, всегда
    /// Ниже этого пояс не имеет смысла: одна-две ячейки не пояс.
    static constexpr u32 HOTBAR_MIN_VISIBLE = 5;

private:
    f32 w_ = 1920.f, h_ = 1080.f;
    theme::Metrics m_{};
    SafeInsets     si_{};
    bool           mirror_ = false;
};

} // namespace ui
