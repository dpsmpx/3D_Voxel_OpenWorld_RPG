#pragma once
#include "../core/types.h"
#include <string>
#include <vector>

namespace config {

// ============================================================
// Языки, доступные в игре.
// ============================================================
enum class Language : u8 {
    English = 0,
    Russian,
    Count
};

const char* languageCode(Language l);
const char* languageName(Language l);

// ============================================================
// Настройки игрока. Живут вне ECS — сохраняются отдельно
// в файле settings.cfg, чтобы не зависеть от слота.
// ============================================================
struct Settings {
    // --- Управление ---
    f32 cameraSensitivity   = 1.2f;    // 0.1 .. 5.0
    bool invertY            = false;
    bool joystickLeftHanded = true;
    f32 joystickRadius      = 140.f;   // 80 .. 220
    f32 joystickDeadzone    = 0.15f;   // 0.05 .. 0.40

    // --- UI ---
    f32 uiScale             = 1.0f;    // 0.75 .. 1.5
    f32 uiOpacity           = 1.0f;    // 0.4 .. 1.0
    bool showFps            = true;
    bool showDebugPos       = false;
    bool showDamageNumbers  = true;

    // --- Аудио (используется Phase 14) ---
    f32 masterVolume        = 1.0f;
    f32 musicVolume         = 0.7f;
    f32 sfxVolume           = 0.9f;

    // --- Игровой процесс ---
    Language language       = Language::English;
    bool     autosaveEnabled = true;
    f32      autosaveInterval = 300.f;    // сек, 60 .. 900

    // --- Рендер (Phase 15 использует) ---
    i32 viewDistance        = 7;         // 4 .. 12
    bool vsync              = true;

    // ---- Валидация/нормализация ----
    void clamp();
    bool isValid() const;

    // ---- Сериализация в файл ----
    bool save(const std::string& path) const;
    bool load(const std::string& path);
};

// ============================================================
// Глобальный доступ к настройкам.
// ============================================================
Settings&       settings();
const Settings& settingsConst();

// Список поддерживаемых разрешений UI (для отладки).
struct ResolutionHint {
    u32 width;
    u32 height;
    const char* name;
};
const std::vector<ResolutionHint>& knownResolutions();

} // namespace config