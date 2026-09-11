#include "settings.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace config {

namespace {

Settings gSettings;

// Простой формат "key=value\n". Парсинг — split по '='.
struct KV {
    char key[64];
    char value[128];
};

bool parseLine(const char* line, KV& out) {
    if (!line) return false;
    if (line[0] == '#' || line[0] == '\0') return false;

    const char* eq = std::strchr(line, '=');
    if (!eq) return false;

    usize klen = (usize)(eq - line);
    if (klen >= sizeof(out.key)) klen = sizeof(out.key) - 1;
    std::memcpy(out.key, line, klen);
    out.key[klen] = '\0';

    const char* v = eq + 1;
    usize vlen = std::strlen(v);
    // Обрезаем \r\n
    while (vlen > 0 && (v[vlen-1] == '\n' || v[vlen-1] == '\r')) --vlen;
    if (vlen >= sizeof(out.value)) vlen = sizeof(out.value) - 1;
    std::memcpy(out.value, v, vlen);
    out.value[vlen] = '\0';
    return true;
}

f32 readF32(const char* v, f32 def) {
    char* end = nullptr;
    f32 r = std::strtof(v, &end);
    if (end == v) return def;
    return r;
}

i32 readI32(const char* v, i32 def) {
    char* end = nullptr;
    long r = std::strtol(v, &end, 10);
    if (end == v) return def;
    return (i32)r;
}

bool readBool(const char* v, bool def) {
    if (!v) return def;
    if (std::strcmp(v, "1") == 0) return true;
    if (std::strcmp(v, "0") == 0) return false;
    if (std::strcmp(v, "true") == 0) return true;
    if (std::strcmp(v, "false") == 0) return false;
    return def;
}

} // namespace

const char* languageCode(Language l) {
    switch (l) {
        case Language::English: return "en";
        case Language::Russian: return "ru";
        default:                return "en";
    }
}

const char* languageName(Language l) {
    switch (l) {
        case Language::English: return "English";
        case Language::Russian: return "Russian";
        default:                return "?";
    }
}

void Settings::clamp() {
    cameraSensitivity  = std::clamp(cameraSensitivity, 0.1f, 5.0f);
    joystickRadius     = std::clamp(joystickRadius, 80.f, 220.f);
    joystickDeadzone   = std::clamp(joystickDeadzone, 0.05f, 0.40f);
    uiScale            = std::clamp(uiScale, 0.75f, 1.5f);
    uiOpacity          = std::clamp(uiOpacity, 0.4f, 1.0f);
    masterVolume       = std::clamp(masterVolume, 0.f, 1.f);
    musicVolume        = std::clamp(musicVolume, 0.f, 1.f);
    sfxVolume          = std::clamp(sfxVolume, 0.f, 1.f);
    autosaveInterval   = std::clamp(autosaveInterval, 60.f, 900.f);
    viewDistance       = std::clamp(viewDistance, 4, 12);

    u8 langIdx = (u8)language;
    if (langIdx >= (u8)Language::Count) language = Language::English;
}

bool Settings::isValid() const {
    if (cameraSensitivity < 0.1f || cameraSensitivity > 5.0f) return false;
    if (uiScale < 0.75f || uiScale > 1.5f) return false;
    if (viewDistance < 4 || viewDistance > 12) return false;
    if ((u8)language >= (u8)Language::Count) return false;
    return true;
}

bool Settings::save(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        LOGE("Settings: не удалось открыть %s для записи", path.c_str());
        return false;
    }

    auto w = [&](const char* key, const char* value) {
        std::fprintf(f, "%s=%s\n", key, value);
    };
    auto wf = [&](const char* key, f32 v) {
        std::fprintf(f, "%s=%.4f\n", key, v);
    };
    auto wi = [&](const char* key, i32 v) {
        std::fprintf(f, "%s=%d\n", key, v);
    };
    auto wb = [&](const char* key, bool v) {
        std::fprintf(f, "%s=%d\n", key, v ? 1 : 0);
    };

    std::fprintf(f, "# VoxelRPG Settings\n");
    std::fprintf(f, "# Format: key=value\n\n");

    std::fprintf(f, "[Input]\n");
    wf("camera_sensitivity", cameraSensitivity);
    wb("invert_y",           invertY);
    wb("joystick_left",      joystickLeftHanded);
    wf("joystick_radius",    joystickRadius);
    wf("joystick_deadzone",  joystickDeadzone);

    std::fprintf(f, "\n[UI]\n");
    wf("ui_scale",           uiScale);
    wf("ui_opacity",         uiOpacity);
    wb("show_fps",           showFps);
    wb("show_debug_pos",     showDebugPos);
    wb("show_damage_numbers",showDamageNumbers);

    std::fprintf(f, "\n[Audio]\n");
    wf("master_volume",      masterVolume);
    wf("music_volume",       musicVolume);
    wf("sfx_volume",         sfxVolume);

    std::fprintf(f, "\n[Game]\n");
    w("language",            languageCode(language));
    wb("autosave_enabled",   autosaveEnabled);
    wf("autosave_interval",  autosaveInterval);

    std::fprintf(f, "\n[Render]\n");
    wi("view_distance",      viewDistance);
    wb("vsync",              vsync);

    std::fclose(f);
    LOGI("Settings сохранены: %s", path.c_str());
    return true;
}

bool Settings::load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        LOGI("Settings: файл %s не найден, используются значения по умолчанию",
             path.c_str());
        return false;
    }

    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        KV kv{};
        if (!parseLine(line, kv)) continue;

        if (std::strcmp(kv.key, "camera_sensitivity") == 0)
            cameraSensitivity = readF32(kv.value, cameraSensitivity);
        else if (std::strcmp(kv.key, "invert_y") == 0)
            invertY = readBool(kv.value, invertY);
        else if (std::strcmp(kv.key, "joystick_left") == 0)
            joystickLeftHanded = readBool(kv.value, joystickLeftHanded);
        else if (std::strcmp(kv.key, "joystick_radius") == 0)
            joystickRadius = readF32(kv.value, joystickRadius);
        else if (std::strcmp(kv.key, "joystick_deadzone") == 0)
            joystickDeadzone = readF32(kv.value, joystickDeadzone);

        else if (std::strcmp(kv.key, "ui_scale") == 0)
            uiScale = readF32(kv.value, uiScale);
        else if (std::strcmp(kv.key, "ui_opacity") == 0)
            uiOpacity = readF32(kv.value, uiOpacity);
        else if (std::strcmp(kv.key, "show_fps") == 0)
            showFps = readBool(kv.value, showFps);
        else if (std::strcmp(kv.key, "show_debug_pos") == 0)
            showDebugPos = readBool(kv.value, showDebugPos);
        else if (std::strcmp(kv.key, "show_damage_numbers") == 0)
            showDamageNumbers = readBool(kv.value, showDamageNumbers);

        else if (std::strcmp(kv.key, "master_volume") == 0)
            masterVolume = readF32(kv.value, masterVolume);
        else if (std::strcmp(kv.key, "music_volume") == 0)
            musicVolume = readF32(kv.value, musicVolume);
        else if (std::strcmp(kv.key, "sfx_volume") == 0)
            sfxVolume = readF32(kv.value, sfxVolume);

        else if (std::strcmp(kv.key, "language") == 0) {
            if (std::strcmp(kv.value, "ru") == 0) language = Language::Russian;
            else if (std::strcmp(kv.value, "en") == 0) language = Language::English;
        }
        else if (std::strcmp(kv.key, "autosave_enabled") == 0)
            autosaveEnabled = readBool(kv.value, autosaveEnabled);
        else if (std::strcmp(kv.key, "autosave_interval") == 0)
            autosaveInterval = readF32(kv.value, autosaveInterval);

        else if (std::strcmp(kv.key, "view_distance") == 0)
            viewDistance = readI32(kv.value, viewDistance);
        else if (std::strcmp(kv.key, "vsync") == 0)
            vsync = readBool(kv.value, vsync);
    }

    std::fclose(f);
    clamp();
    LOGI("Settings загружены: %s", path.c_str());
    return true;
}

Settings& settings() { return gSettings; }
const Settings& settingsConst() { return gSettings; }

const std::vector<ResolutionHint>& knownResolutions() {
    static const std::vector<ResolutionHint> v = {
        {  720, 1280, "HD"       },
        { 1080, 1920, "FHD"      },
        { 1080, 2340, "FHD+ 19.5:9" },
        { 1440, 2560, "QHD"      },
        { 1440, 3200, "QHD+"     },
    };
    return v;
}

} // namespace config
