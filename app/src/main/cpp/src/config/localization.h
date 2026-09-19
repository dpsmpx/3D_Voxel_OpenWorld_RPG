/**
 * @file localization.h
 * @brief Настройки, локализация, счётчик игрового времени.
 */
#pragma once
#include "../core/types.h"
#include "settings.h"
#include <string>
#include <unordered_map>

namespace config {

/// Ключ локализации. Все ключи собраны в одном enum, чтобы
/// компилятор ловил опечатки.
enum class StrKey : u16 {
    None = 0,

    // --- Общие ---
    Ok, Cancel, Yes, No, Back, Close, Save, Load, Delete, Reset, Apply,
    On, Off, Volume, Sensitivity, Language_, Enabled, Disabled,

    // --- Меню ---
    Menu_NewGame, Menu_Continue, Menu_Settings, Menu_Quit,
    Menu_Pause, Menu_Resume, Menu_Inventory, Menu_Skills,
    Menu_Attributes, Menu_Quests, Menu_Reputation, Menu_SaveLoad,
    Menu_Crafting, Menu_Trade,

    // --- HUD ---
    Hud_Health, Hud_Mana, Hud_Stamina, Hud_Gold, Hud_Xp, Hud_Level,
    Hud_Resonance, Hud_FinisherReady, Hud_ActiveQuests,

    // --- Инвентарь ---
    Inv_Title, Inv_Use, Inv_Drop, Inv_Sort,
    Inv_Equipped, Inv_Hotbar, Inv_Hint_Tap, Inv_Hint_LongTap,

    // --- Крафт ---
    Craft_Title, Craft_Workbench, Craft_Anvil, Craft_Alchemy,
    Craft_ByHand,
    Craft_Ingredients, Craft_Required, Craft_Button, Craft_Select,

    // --- Торговля ---
    Trade_Title, Trade_Buy, Trade_Sell, Trade_Price, Trade_NothingToBuy,

    // --- Квесты ---
    Quest_Title, Quest_Active, Quest_Completed, Quest_None,
    Quest_Rewards,

    // --- Репутация ---
    Rep_Title, Rep_Hated, Rep_Hostile, Rep_Unfriendly, Rep_Neutral,
    Rep_Friendly, Rep_Honored, Rep_Exalted,

    // --- Настройки ---
    Settings_Title, Settings_Input, Settings_Ui, Settings_Audio,
    Settings_Game, Settings_Render, Settings_ResetAll,
    Settings_CameraSens, Settings_InvertX, Settings_InvertY,
    Settings_JoystickLeft,
    Settings_JoystickRadius, Settings_JoystickDeadzone,
    Settings_JoystickOpacity, Settings_ButtonScale, Settings_ButtonOpacity,
    Settings_ButtonLayout, Settings_LayoutHint, Settings_ResetLayout,
    Settings_UiScale, Settings_UiOpacity, Settings_ShowFps,
    Settings_ShowDebug, Settings_Master, Settings_Music, Settings_Sfx,
    Settings_Language, Settings_Autosave, Settings_AutosaveInterval,
    Settings_ViewDistance, Settings_UnlimitedFps,

    // --- Уведомления ---
    Notif_LevelUp, Notif_Crafted, Notif_Saved, Notif_Loaded,
    Notif_Deleted, Notif_RepGained, Notif_Loading,
    Notif_Died, Notif_Respawned, Notif_WellBound,
    // Логова: игрок должен понять, почему вокруг вдруг одни
    // волки, — иначе это читается как поломка спавна.
    Notif_LairWolves, Notif_LairSkeletons,
    Notif_LairGoblins, Notif_LairSlimes,

    // --- Подсказка взаимодействия ---
    // Её не было вовсе, и это стоило игроку трёх экранов: близость
    // станка и алтаря считалась каждый кадр, а показать было нечем.
    Hud_Use, Ench_Altar,

    Count
};

constexpr u16 STR_KEY_COUNT = (u16)StrKey::Count;

/// Система локализации. Хранит таблицу строк для текущего языка.
/// Инициализируется при старте, переключается в настройках.
class Localization {
public:
    static Localization& instance();

    /// Установить язык.
    void setLanguage(Language l);
    Language language() const { return current_; }

    /// Получить строку.
    const char* get(StrKey k) const;

    /// Для отладочной/динамической подстановки: format "HP: %d/%d".
    /// Возвращает результат в internal-буфере (не потокобезопасно).
    const char* format(StrKey k, ...);

private:
    Localization();
    void buildEnglish();
    void buildRussian();

    Language current_ = Language::English;
    const char* table_[STR_KEY_COUNT] = {};

    // Двойной буфер — чтобы UI мог держать 2 строки одновременно.
    mutable char buf1_[256] = {};
    mutable char buf2_[256] = {};
    mutable int  bufToggle_ = 0;

    std::unordered_map<std::string, const char*> byName_;
};

inline Localization& L() { return Localization::instance(); }

/// Удобная короткая форма.
inline const char* T(StrKey k) { return Localization::instance().get(k); }

} // namespace config
