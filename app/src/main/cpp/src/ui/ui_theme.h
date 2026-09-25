/**
 * @file ui_theme.h
 * @brief Дизайн-система интерфейса: единицы, цвет, шрифт, размеры.
 *
 * Исполняемая половина docs/UI_DESIGN_SYSTEM.md. Числа здесь и там
 * обязаны совпадать — за этим следит проверка на хосте.
 *
 * Правило одно: ни один экран не заводит собственный цвет, отступ или
 * масштаб текста. До этого файла в ui_system.cpp было 193 литерала
 * rgba со 131 уникальным цветом и десять ступеней шрифта.
 */
#pragma once
#include "ui_types.h"

namespace ui::theme {

// ============================================================
// 1. Единица длины: dp, а не пиксель
// ============================================================
//
// Размер цели касания задаёт палец, а он одного размера на любом
// экране. Значит переводить надо через ПЛОТНОСТЬ, а не через число
// пикселей.
//
// Прежний hudScale = screenH / 1080 считал именно от числа пикселей.
// Два телефона одного физического размера, 1080 и 720 по высоте,
// получали кнопки, различающиеся в полтора раза. На рабочем телефоне
// (2306x1080, ~397 ppi) это дало цели по 20..36 dp при норме 48:
// кнопка меню HUD — 3.2 мм при подушечке пальца 8..10 мм.

constexpr f32 DENSITY_BASE_DPI  = 160.f;   ///< dp определён при 160 dpi
constexpr f32 DENSITY_FALLBACK  = 2.0f;    ///< xhdpi, если система молчит
constexpr f32 USER_SCALE_MIN    = 0.85f;
constexpr f32 USER_SCALE_MAX    = 1.30f;

/// Общий масштаб интерфейса относительно размеров в dp.
///
/// Все размеры в этом файле и в раскладке записаны по меркам Android
/// — цель касания 48 dp, ряд 56, главная кнопка 72. На телефоне в
/// руках они выходили громоздкими: ряд меню занимал седьмую часть
/// высоты экрана, окна не помещали и половины того, что могли бы, и
/// кнопки упирались в пояс. Мерки оставлены прежними — по ним
/// считаются все соотношения, — а на экран они идут с этим
/// множителем: цель касания — около 38 dp, это по-прежнему шесть
/// миллиметров, палец попадает.
constexpr f32 UI_BASE_SCALE = 0.80f;

/// Сколько dp раскладки должно помещаться по короткой стороне экрана.
///
/// Плотность говорит, сколько пикселей в миллиметре, но не сколько
/// миллиметров у экрана. Маленький плотный экран давал раскладке
/// 300 dp высоты и меньше — и окна, рассчитанные на 360, уезжали за
/// край. Такой экран ужимает интерфейс ещё, пока высоты не хватит, —
/// но не ниже FIT_MIN от обычного: мельче палец уже не попадает.
constexpr f32 FIT_SHORT_SIDE_DP = 360.f;
constexpr f32 FIT_MIN           = 0.75f;

/// Перевод dp в пиксели экрана.
///
/// Множителей ровно три, и каждый назван: плотность экрана, настройка
/// игрока uiScale и масштаб под экран (`fit`: UI_BASE_SCALE, ужатый
/// для маленького экрана). Других общих множителей быть не должно:
/// каждый следующий делает размер на экране непредсказуемым.
struct Metrics {
    f32 pxPerDp   = DENSITY_FALLBACK;
    f32 userScale = 1.f;
    f32 fit       = UI_BASE_SCALE;

    constexpr f32 dp(f32 v) const { return v * pxPerDp * userScale * fit; }

    /// AConfiguration_getDensity возвращает dpi, либо 0 (не сообщает),
    /// либо ACONFIGURATION_DENSITY_ANY (0xFFFE). Оба служебных
    /// значения означают «не знаю» — берём запасное.
    static constexpr Metrics fromDensityDpi(i32 dpi, f32 userScale = 1.f) {
        const f32 s = userScale < USER_SCALE_MIN ? USER_SCALE_MIN
                    : (userScale > USER_SCALE_MAX ? USER_SCALE_MAX : userScale);
        const bool known = dpi > 0 && dpi < 0xFFFE;
        return Metrics{ known ? (f32)dpi / DENSITY_BASE_DPI : DENSITY_FALLBACK, s,
                        UI_BASE_SCALE };
    }

    /// Те же мерки, ужатые под экран w×h пикселей: по короткой стороне
    /// должно помещаться FIT_SHORT_SIDE_DP.
    constexpr Metrics fittedTo(f32 w, f32 h) const {
        Metrics m = *this;
        const f32 shortPx = w < h ? w : h;
        const f32 perDp = pxPerDp * userScale * fit;
        if (shortPx <= 0.f || perDp <= 0.f) return m;
        const f32 units = shortPx / perDp;
        if (units < FIT_SHORT_SIDE_DP) {
            f32 k = units / FIT_SHORT_SIDE_DP;
            if (k < FIT_MIN) k = FIT_MIN;
            m.fit = fit * k;
        }
        return m;
    }
};

// ============================================================
// 2. Размеры касания
// ============================================================
//
// TOUCH_MIN — не рекомендация, а нижняя граница: всё, что нажимается,
// не может быть меньше. Проверяется автоматически.

constexpr f32 TOUCH_MIN_DP      = 48.f;  ///< ниже палец промахивается
constexpr f32 TOUCH_REGULAR_DP  = 56.f;  ///< ряд, кнопка меню, ячейка
constexpr f32 TOUCH_PRIMARY_DP  = 72.f;  ///< главное действие экрана
constexpr f32 TOUCH_GAP_DP      =  8.f;  ///< зазор между соседними зонами

// Круглые кнопки поверх мира.
constexpr f32 PAD_BUTTON_MIN_DP = 64.f;
constexpr f32 PAD_BUTTON_MAX_DP = 96.f;

// ============================================================
// 3. Отступы: сетка 4 dp
// ============================================================
constexpr f32 SPACE_XS_DP  =  4.f;
constexpr f32 SPACE_S_DP   =  8.f;
constexpr f32 SPACE_M_DP   = 12.f;
constexpr f32 SPACE_L_DP   = 16.f;
constexpr f32 SPACE_XL_DP  = 24.f;
constexpr f32 SPACE_XXL_DP = 32.f;

/// Внутренний отступ панели и положение её заголовка.
constexpr f32 PANEL_PAD_DP = SPACE_L_DP;

// ============================================================
// 4. Форма: срез угла, а не скругление
// ============================================================
//
// Дуга стоит десяток треугольников на угол, срез — ровно один. И он
// гранёный, как сам мир: не прямой угол Minecraft и не полное
// скругление обычной мобильной игры.
constexpr f32 CHAMFER_PANEL_DP = 6.f;
constexpr f32 CHAMFER_CELL_DP  = 4.f;
constexpr f32 CHAMFER_NONE_DP  = 0.f;   ///< полосы, разделители

/// Толщина рамок.
constexpr f32 STROKE_DP          = 2.f;
constexpr f32 STROKE_SELECTED_DP = 3.f;  ///< выбранное толще — признак помимо цвета

// ============================================================
// 5. Цвет
// ============================================================
//
// Контраст измерен по WCAG и закреплён проверкой. Пороги: основной
// текст >= 4.5, вторичный и недоступный >= 3.0.

// -- основа --
constexpr UiColor Ink         = rgba(0x0E, 0x12, 0x19, 255);
constexpr UiColor Panel       = rgba(0x1B, 0x21, 0x2C, 255);
constexpr UiColor PanelRaised = rgba(0x27, 0x30, 0x3F, 255);
constexpr UiColor Stroke      = rgba(0x3C, 0x46, 0x57, 255);

// -- текст --
constexpr UiColor TextPrimary   = rgba(0xF2, 0xF5, 0xFA, 255);
constexpr UiColor TextSecondary = rgba(0xAA, 0xB4, 0xC6, 255);
// #626C7C давал на PanelRaised 2.5 и был отвергнут: недоступное всё
// равно надо прочитать.
constexpr UiColor TextDisabled  = rgba(0x72, 0x7C, 0x8C, 255);

// -- акцент и смысл --
constexpr UiColor Accent        = rgba(0xF2, 0xB3, 0x3D, 255);
constexpr UiColor AccentPressed = rgba(0xFF, 0xD4, 0x78, 255);
constexpr UiColor Danger        = rgba(0xC4, 0x44, 0x3C, 255);
constexpr UiColor Success       = rgba(0x4F, 0xA8, 0x4A, 255);

// -- ресурсы: заливка и своё тёмное ложе того же тона --
constexpr UiColor Hp    = rgba(0xD9, 0x48, 0x3F, 255);
constexpr UiColor HpBed = rgba(0x2A, 0x14, 0x14, 255);
constexpr UiColor Mp    = rgba(0x3D, 0x7F, 0xD9, 255);
constexpr UiColor MpBed = rgba(0x14, 0x1E, 0x2E, 255);
constexpr UiColor Sp    = rgba(0x5F, 0xB8, 0x4A, 255);
constexpr UiColor SpBed = rgba(0x16, 0x2A, 0x16, 255);
constexpr UiColor Xp    = rgba(0xA8, 0x68, 0xE0, 255);
constexpr UiColor XpBed = rgba(0x24, 0x16, 0x34, 255);

// -- прозрачность --
constexpr u8 ALPHA_SCRIM    = 184;  ///< 0.72 — затемнение под модальным
constexpr u8 ALPHA_PANEL    = 240;  ///< 0.94 — панель поверх мира
constexpr u8 ALPHA_HUD      = 219;  ///< 0.86 — HUD поверх мира
constexpr u8 ALPHA_DISABLED = 115;  ///< 0.45 — выключенный элемент

// ============================================================
// 6. Типографика
// ============================================================
//
// Шрифт растровый 5x7, только заглавные ASCII 32..95. Ступеней ровно
// пять; промежуточных значений нет. Не влезает — сокращается текст,
// а не шрифт.
constexpr f32 TEXT_DISPLAY = 3.0f;  ///< событие во весь экран
constexpr f32 TEXT_TITLE   = 2.2f;  ///< заголовок экрана
constexpr f32 TEXT_BODY    = 2.0f;  ///< кнопки, ряды, основной текст
constexpr f32 TEXT_LABEL   = 1.6f;  ///< подписи, вкладки
constexpr f32 TEXT_CAPTION = 1.3f;  ///< число в ячейке, служебное

constexpr f32 TEXT_SCALES[] = {
    TEXT_CAPTION, TEXT_LABEL, TEXT_BODY, TEXT_TITLE, TEXT_DISPLAY
};
constexpr u32 TEXT_SCALE_COUNT =
    (u32)(sizeof(TEXT_SCALES) / sizeof(TEXT_SCALES[0]));

/// Высота глифа шрифта в точках при данной ступени.
constexpr f32 textHeight(f32 scale) { return 7.f * scale; }
/// Межстрочное расстояние — 1.4 от высоты ступени.
constexpr f32 lineHeight(f32 scale) { return textHeight(scale) * 1.4f; }

// ============================================================
// 7. Движение
// ============================================================
//
// Ввод никогда не ждёт анимации: действие происходит сразу, движение
// только показывает результат. Дольше 250 мс — ничего.
constexpr f32 ANIM_PRESS_S     = 0.f;    ///< нажатие видно в том же кадре
constexpr f32 ANIM_PANEL_IN_S  = 0.12f;
constexpr f32 ANIM_PANEL_OUT_S = 0.10f;
constexpr f32 ANIM_TOAST_IN_S  = 0.15f;
constexpr f32 ANIM_TOAST_OUT_S = 0.25f;
constexpr f32 ANIM_BAR_S       = 0.20f;  ///< полоса догоняет значение
constexpr f32 ANIM_SELECT_S    = 0.08f;
constexpr f32 ANIM_MAX_S       = 0.25f;  ///< потолок для любой анимации

constexpr f32 PANEL_IN_SCALE_FROM = 0.96f;

// ============================================================
// 8. Уведомления
// ============================================================
enum class NotifyPriority : u8 { Low = 0, Normal, High };

constexpr f32 NOTIFY_LOW_S    = 1.5f;
constexpr f32 NOTIFY_NORMAL_S = 2.5f;
constexpr f32 NOTIFY_HIGH_S   = 3.0f;
constexpr u32 NOTIFY_MAX_VISIBLE = 2;   ///< новое не затирает, а ждёт

constexpr f32 notifyDuration(NotifyPriority p) {
    return p == NotifyPriority::High   ? NOTIFY_HIGH_S
         : p == NotifyPriority::Normal ? NOTIFY_NORMAL_S
                                       : NOTIFY_LOW_S;
}

// ============================================================
// 9. Свободный центр экрана
// ============================================================
//
// Прямоугольник по центру, который не занимает ничто, кроме прицела и
// подсказки взаимодействия.
constexpr f32 HUD_CLEAR_W_FRAC = 0.60f;
constexpr f32 HUD_CLEAR_H_FRAC = 0.50f;

} // namespace ui::theme
