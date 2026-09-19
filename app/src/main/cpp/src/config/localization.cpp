/**
 * @file localization.cpp
 * @brief Настройки, локализация, счётчик игрового времени.
 */
#include "localization.h"
#include "../core/log.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace config {

// ============================================================
// Английская таблица
// ============================================================
namespace en {

static const char* EN[STR_KEY_COUNT] = {
    /* None */              "?",

    /* Ok */                "OK",
    /* Cancel */            "Cancel",
    /* Yes */               "Yes",
    /* No */                "No",
    /* Back */              "Back",
    /* Close */             "Close",
    /* Save */              "Save",
    /* Load */              "Load",
    /* Delete */            "Delete",
    /* Reset */             "Reset",
    /* Apply */             "Apply",
    /* On */                "On",
    /* Off */               "Off",
    /* Volume */            "Volume",
    /* Sensitivity */       "Sensitivity",
    /* Language_ */         "Language",
    /* Enabled */           "Enabled",
    /* Disabled */          "Disabled",

    /* Menu_NewGame */      "New Game",
    /* Menu_Continue */     "Continue",
    /* Menu_Settings */     "Settings",
    /* Menu_Quit */         "Quit",
    /* Menu_Pause */        "PAUSED",
    /* Menu_Resume */       "RESUME",
    /* Menu_Inventory */    "INVENTORY",
    /* Menu_Skills */       "SKILLS",
    /* Menu_Attributes */   "ATTRIBUTES",
    /* Menu_Quests */       "QUEST LOG",
    /* Menu_Reputation */   "REPUTATION",
    /* Menu_SaveLoad */     "SAVE / LOAD",
    /* Menu_Crafting */     "CRAFTING",
    /* Menu_Trade */        "TRADE",

    /* Hud_Health */        "HP",
    /* Hud_Mana */          "MP",
    /* Hud_Stamina */       "SP",
    /* Hud_Gold */          "Gold",
    /* Hud_Xp */            "XP",
    /* Hud_Level */         "LV",
    /* Hud_Resonance */     "RESONANCE",
    /* Hud_FinisherReady */ "FINISHER READY!",
    /* Hud_ActiveQuests */  "Active quests",

    /* Inv_Title */         "INVENTORY",
    /* Inv_Use */           "Use",
    /* Inv_Drop */          "Drop",
    /* Inv_Sort */          "Sort",
    /* Inv_Equipped */      "EQUIPPED",
    /* Inv_Hotbar */        "HOTBAR",
    /* Inv_Hint_Tap */      "Tap to use",
    /* Inv_Hint_LongTap */  "Long tap to drop",

    /* Craft_Title */       "CRAFTING",
    /* Craft_Workbench */   "Workbench",
    /* Craft_Anvil */       "Anvil",
    /* Craft_Alchemy */     "Alchemy Table",
    /* Craft_ByHand */      "By hand",
    /* Craft_Ingredients */ "Ingredients",
    /* Craft_Required */    "Required",
    /* Craft_Button */      "CRAFT",
    /* Craft_Select */      "Select a recipe",

    /* Trade_Title */       "TRADE",
    /* Trade_Buy */         "BUY",
    /* Trade_Sell */        "SELL",
    /* Trade_Price */       "Price",
    /* Trade_NothingToBuy */ "Nothing for sale",

    /* Quest_Title */       "QUEST LOG",
    /* Quest_Active */      "Active Quests",
    /* Quest_Completed */   "Completed",
    /* Quest_None */        "No active quests.",
    /* Quest_Rewards */    "Rewards",

    /* Rep_Title */         "REPUTATION",
    /* Rep_Hated */         "Hated",
    /* Rep_Hostile */       "Hostile",
    /* Rep_Unfriendly */    "Unfriendly",
    /* Rep_Neutral */       "Neutral",
    /* Rep_Friendly */      "Friendly",
    /* Rep_Honored */       "Honored",
    /* Rep_Exalted */       "Exalted",

    /* Settings_Title */    "SETTINGS",
    /* Settings_Input */    "Input",
    /* Settings_Ui */       "UI",
    /* Settings_Audio */    "Audio",
    /* Settings_Game */     "Game",
    /* Settings_Render */   "Render",
    /* Settings_ResetAll */ "Reset to defaults",
    /* Settings_CameraSens */"Camera Sensitivity",
    /* Settings_InvertX */  "Invert X",
    /* Settings_InvertY */  "Invert Y",
    /* Settings_JoystickLeft */"Joystick Left Side",
    /* Settings_JoystickRadius */"Joystick Radius",
    /* Settings_JoystickDeadzone */"Joystick Deadzone",
    /* Settings_JoystickOpacity */"Joystick Opacity",
    /* Settings_ButtonScale */   "Button Size",
    /* Settings_ButtonOpacity */ "Button Opacity",
    /* Settings_ButtonLayout */  "Move Buttons",
    /* Settings_LayoutHint */    "Drag a button to move it",
    /* Settings_ResetLayout */   "Reset Layout",
    /* Settings_UiScale */  "UI Scale",
    /* Settings_UiOpacity */"UI Opacity",
    /* Settings_ShowFps */  "Show FPS",
    /* Settings_ShowDebug */"Show Debug Info",
    /* Settings_Master */   "Master Volume",
    /* Settings_Music */    "Music Volume",
    /* Settings_Sfx */      "SFX Volume",
    /* Settings_Language */ "Language",
    /* Settings_Autosave */ "Autosave",
    /* Settings_AutosaveInterval */"Autosave Interval",
    /* Settings_ViewDistance */"View Distance",
    /* Settings_UnlimitedFps */ "Unlimited FPS",

    /* Notif_LevelUp */     "LEVEL UP!",
    /* Notif_Crafted */     "Crafted!",
    /* Notif_Saved */       "Saved",
    /* Notif_Loaded */      "Loaded",
    /* Notif_Deleted */     "Deleted",
    /* Notif_RepGained */   "Reputation changed",
    /* Notif_Loading */     "Generating world...",
    /* Notif_Died */        "You died",
    /* Notif_Respawned */   "Back at the well",
    /* Notif_WellBound */   "Well remembered",
    /* Notif_LairWolves */    "A wolf den — only wolves roam here",
    /* Notif_LairSkeletons */ "Bone fields — only skeletons roam here",
    /* Notif_LairGoblins */   "A goblin camp — only goblins roam here",
    /* Notif_LairSlimes */    "A slime bog — only slimes roam here",
    /* Notif_Fall */          "A hard landing",
    /* Notif_Trap */          "A trap!",
    /* Notif_Treasure */      "A cache, and it is yours",

    /* Hud_Use */           "USE",
    /* Ench_Altar */        "Enchant Altar",
};

} // namespace en

// ============================================================
// Русская таблица
// ============================================================
namespace ru {

static const char* RU[STR_KEY_COUNT] = {
    /* None */              "?",

    /* Ok */                "ОК",
    /* Cancel */            "Отмена",
    /* Yes */               "Да",
    /* No */                "Нет",
    /* Back */              "Назад",
    /* Close */             "Закрыть",
    /* Save */              "Сохранить",
    /* Load */              "Загрузить",
    /* Delete */            "Удалить",
    /* Reset */             "Сбросить",
    /* Apply */             "Применить",
    /* On */                "Вкл",
    /* Off */               "Выкл",
    /* Volume */            "Громкость",
    /* Sensitivity */       "Чувствительность",
    /* Language_ */         "Язык",
    /* Enabled */           "Включено",
    /* Disabled */          "Отключено",

    /* Menu_NewGame */      "Новая игра",
    /* Menu_Continue */     "Продолжить",
    /* Menu_Settings */     "Настройки",
    /* Menu_Quit */         "Выход",
    /* Menu_Pause */        "ПАУЗА",
    /* Menu_Resume */       "ПРОДОЛЖИТЬ",
    /* Menu_Inventory */    "ИНВЕНТАРЬ",
    /* Menu_Skills */       "НАВЫКИ",
    /* Menu_Attributes */   "АТРИБУТЫ",
    /* Menu_Quests */       "ЖУРНАЛ",
    /* Menu_Reputation */   "РЕПУТАЦИЯ",
    /* Menu_SaveLoad */     "СОХР / ЗАГР",
    /* Menu_Crafting */     "КРАФТ",
    /* Menu_Trade */        "ТОРГОВЛЯ",

    /* Hud_Health */        "ЗД",
    /* Hud_Mana */          "МН",
    /* Hud_Stamina */       "ВН",
    /* Hud_Gold */          "Золото",
    /* Hud_Xp */            "ОП",
    /* Hud_Level */         "УР",
    /* Hud_Resonance */     "РЕЗОНАНС",
    /* Hud_FinisherReady */ "ФИНИШЕР ГОТОВ!",
    /* Hud_ActiveQuests */  "Активные квесты",

    /* Inv_Title */         "ИНВЕНТАРЬ",
    /* Inv_Use */           "Использовать",
    /* Inv_Drop */          "Выбросить",
    /* Inv_Sort */          "Сортировать",
    /* Inv_Equipped */      "ЭКИПИРОВАНО",
    /* Inv_Hotbar */        "ХОТБАР",
    /* Inv_Hint_Tap */      "Тап — использовать",
    /* Inv_Hint_LongTap */  "Долгий тап — выбросить",

    /* Craft_Title */       "КРАФТ",
    /* Craft_Workbench */   "Верстак",
    /* Craft_Anvil */       "Наковальня",
    /* Craft_Alchemy */     "Алхимический стол",
    /* Craft_ByHand */      "На ходу",
    /* Craft_Ingredients */ "Ингредиенты",
    /* Craft_Required */    "Требуется",
    /* Craft_Button */      "СОЗДАТЬ",
    /* Craft_Select */      "Выберите рецепт",

    /* Trade_Title */       "ТОРГОВЛЯ",
    /* Trade_Buy */         "КУПИТЬ",
    /* Trade_Sell */        "ПРОДАТЬ",
    /* Trade_Price */       "Цена",
    /* Trade_NothingToBuy */ "Торговать нечем",

    /* Quest_Title */       "ЖУРНАЛ КВЕСТОВ",
    /* Quest_Active */      "Активные",
    /* Quest_Completed */   "Завершённые",
    /* Quest_None */        "Нет активных квестов.",
    /* Quest_Rewards */    "Награда",

    /* Rep_Title */         "РЕПУТАЦИЯ",
    /* Rep_Hated */         "Ненависть",
    /* Rep_Hostile */       "Враждебность",
    /* Rep_Unfriendly */    "Недоверие",
    /* Rep_Neutral */       "Нейтрально",
    /* Rep_Friendly */      "Дружелюбие",
    /* Rep_Honored */       "Уважение",
    /* Rep_Exalted */       "Превознесение",

    /* Settings_Title */    "НАСТРОЙКИ",
    /* Settings_Input */    "Управление",
    /* Settings_Ui */       "Интерфейс",
    /* Settings_Audio */    "Аудио",
    /* Settings_Game */     "Игра",
    /* Settings_Render */   "Графика",
    /* Settings_ResetAll */ "Сбросить всё",
    /* Settings_CameraSens */"Чувствительность камеры",
    /* Settings_InvertX */  "Инверсия по X",
    /* Settings_InvertY */  "Инверсия по Y",
    /* Settings_JoystickLeft */"Джойстик слева",
    /* Settings_JoystickRadius */"Радиус джойстика",
    /* Settings_JoystickDeadzone */"Мёртвая зона джойстика",
    /* Settings_JoystickOpacity */"Прозрачность джойстика",
    /* Settings_ButtonScale */   "Размер кнопок",
    /* Settings_ButtonOpacity */ "Прозрачность кнопок",
    /* Settings_ButtonLayout */  "Перемещение кнопок",
    /* Settings_LayoutHint */    "Перетащите кнопку, чтобы сдвинуть",
    /* Settings_ResetLayout */   "Сбросить раскладку",
    /* Settings_UiScale */  "Масштаб UI",
    /* Settings_UiOpacity */"Прозрачность UI",
    /* Settings_ShowFps */  "Показывать FPS",
    /* Settings_ShowDebug */"Отладочная информация",
    /* Settings_Master */   "Общая громкость",
    /* Settings_Music */    "Музыка",
    /* Settings_Sfx */      "Звуки",
    /* Settings_Language */ "Язык",
    /* Settings_Autosave */ "Автосохранение",
    /* Settings_AutosaveInterval */"Интервал автосейва",
    /* Settings_ViewDistance */"Дальность прорисовки",
    /* Settings_UnlimitedFps */ "Без ограничения кадров",

    /* Notif_LevelUp */     "НОВЫЙ УРОВЕНЬ!",
    /* Notif_Crafted */     "Создано!",
    /* Notif_Saved */       "Сохранено",
    /* Notif_Loaded */      "Загружено",
    /* Notif_Deleted */     "Удалено",
    /* Notif_RepGained */   "Репутация изменена",
    /* Notif_Loading */     "Генерация мира...",
    /* Notif_Died */        "Вы погибли",
    /* Notif_Respawned */   "Снова у колодца",
    /* Notif_WellBound */   "Колодец запомнен",
    /* Notif_LairWolves */    "Волчье логово — тут водятся одни волки",
    /* Notif_LairSkeletons */ "Костяное поле — тут водятся одни скелеты",
    /* Notif_LairGoblins */   "Гоблинская стоянка — тут одни гоблины",
    /* Notif_LairSlimes */    "Слизневая топь — тут водятся одни слизни",
    /* Notif_Fall */          "Тяжёлое приземление",
    /* Notif_Trap */          "Ловушка!",
    /* Notif_Treasure */      "Тайник, и он ваш",

    /* Hud_Use */           "ИСПОЛЬЗОВАТЬ",
    /* Ench_Altar */        "Алтарь зачарования",
};

} // namespace ru

// ============================================================
// Localization
// ============================================================
Localization::Localization() {
    std::memcpy(table_, en::EN, sizeof(table_));
    current_ = Language::English;

    // Регистрируем текстовые имена для отладки.
    byName_["Ok"]   = en::EN[(u16)StrKey::Ok];
    byName_["Save"] = en::EN[(u16)StrKey::Save];
    byName_["Load"] = en::EN[(u16)StrKey::Load];
}

Localization& Localization::instance() {
    static Localization inst;
    return inst;
}

void Localization::buildEnglish() {
    std::memcpy(table_, en::EN, sizeof(table_));
}

void Localization::buildRussian() {
    std::memcpy(table_, ru::RU, sizeof(table_));
}

void Localization::setLanguage(Language l) {
    if (l == current_) return;
    current_ = l;

    switch (l) {
        case Language::Russian: buildRussian(); break;
        case Language::English:
        default:                buildEnglish(); break;
    }

    LOGI("Localization: язык установлен %s", languageCode(l));
}

// Таблицы индексируются StrKey напрямую: если забыть строку при
// добавлении ключа, ошибка вылезет здесь, а не пустой надписью в игре.
static_assert(sizeof(en::EN) / sizeof(en::EN[0]) == STR_KEY_COUNT,
              "В английской таблице не столько строк, сколько ключей StrKey");
static_assert(sizeof(ru::RU) / sizeof(ru::RU[0]) == STR_KEY_COUNT,
              "В русской таблице не столько строк, сколько ключей StrKey");

const char* Localization::get(StrKey k) const {
    u16 i = (u16)k;
    if (i >= STR_KEY_COUNT) return "?";
    const char* s = table_[i];
    return s ? s : "?";
}

const char* Localization::format(StrKey k, ...) {
    const char* tmpl = get(k);
    if (!tmpl) return "";

    char* target = (bufToggle_ == 0) ? buf1_ : buf2_;
    bufToggle_ = 1 - bufToggle_;

    va_list args;
    va_start(args, k);
    std::vsnprintf(target, 256, tmpl, args);
    va_end(args);

    return target;
}

} // namespace config
