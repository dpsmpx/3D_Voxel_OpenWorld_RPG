/**
 * @file settings.h
 * @brief Настройки, локализация, счётчик игрового времени.
 */
#pragma once
#include "../core/types.h"
#include <string>
#include <vector>

namespace config {

/// Языки, доступные в игре.
/// Экранные кнопки, положение которых игрок может менять.
/// Порядок обязан совпадать с порядком регистрации в Engine::setupButtons.
enum ButtonSlot : u8 {
    Btn_Attack = 0,
    Btn_Finisher,
    Btn_Jump,
    Btn_Sprint,
    Btn_Break,
    Btn_Place,
    Btn_Interact,
    Btn_UseItem,
    Btn_Camera,
    Btn_SlotCount,
};
static_assert((u32)Btn_SlotCount == 9, "BUTTON_SLOTS должен совпадать с Btn_SlotCount");

enum class Language : u8 {
    English = 0,
    Russian,
    Count
};

/// Двухбуквенный код языка ("en", "ru") — то, что пишется в settings.cfg.
const char* languageCode(Language l);
/// Название языка на нём самом — для списка в настройках.
const char* languageName(Language l);

/// Настройки игрока. Живут вне ECS — сохраняются отдельно
/// в файле settings.cfg, чтобы не зависеть от слота.
struct Settings {
    /// --- Управление (ТЗ 5.2) ---
    f32  cameraSensitivity  = 1.2f;    // 0.1 .. 5.0
    bool invertX            = false;
    bool invertY            = false;
    bool joystickLeftHanded = true;
    f32  joystickRadius     = 140.f;   // 80 .. 220
    f32  joystickDeadzone   = 0.15f;   // 0.05 .. 0.40
    f32  joystickOpacity    = 0.6f;    // 0.2 .. 1.0

    /// Общий масштаб экранных кнопок, 0.6 .. 1.6.
    f32  buttonScale        = 1.f;
    /// Прозрачность экранных кнопок, 0.2 .. 1.0.
    f32  buttonOpacity      = 0.85f;

    /// Пользовательские сдвиги кнопок в NDC. Индекс — ButtonSlot,
    /// см. ниже; позволяет свободно раскладывать кнопки по экрану.
    static constexpr u32 BUTTON_SLOTS = 9;
    f32  buttonOffsetX[BUTTON_SLOTS] = {};
    f32  buttonOffsetY[BUTTON_SLOTS] = {};

    /// --- UI ---
    f32 uiScale             = 1.0f;    // 0.75 .. 1.5
    f32 uiOpacity           = 1.0f;    // 0.4 .. 1.0
    bool showFps            = true;
    bool showDebugPos       = false;
    bool showDamageNumbers  = true;

    /// --- Аудио (используется Phase 14) ---
    f32 masterVolume        = 1.0f;
    f32 musicVolume         = 0.7f;
    f32 sfxVolume           = 0.9f;

    /// --- Игровой процесс ---
    Language language       = Language::English;
    bool     autosaveEnabled = true;
    f32      autosaveInterval = 300.f;    // сек, 60 .. 900

    /// --- Рендер (Phase 15 использует) ---
    i32 viewDistance        = 7;         // 4 .. 12
    bool vsync              = true;

    /// ---- Валидация/нормализация ----
    void clamp();
    bool isValid() const;

    /// ---- Сериализация в файл ----
    bool save(const std::string& path) const;
    bool load(const std::string& path);
};

/// Глобальный доступ к настройкам.
Settings&       settings();
/// Настройки только для чтения. Используйте там, где менять их не нужно:
/// сигнатура сама не даст случайно записать.
const Settings& settingsConst();

/// Список поддерживаемых разрешений UI (для отладки).
struct ResolutionHint {
    u32 width;
    u32 height;
    const char* name;
};
/// Справочник типовых разрешений и соотношений сторон.
/// Нужен, чтобы проверять раскладку UI на 16:9, 18:9 и 20:9.
const std::vector<ResolutionHint>& knownResolutions();

} // namespace config
