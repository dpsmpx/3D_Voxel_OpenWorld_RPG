/**
 * @file ui_system.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "ui_renderer.h"
#include "ui_context.h"
#include "drag_drop.h"
#include "scroll.h"
#include "minimap.h"
#include "../player/player.h"
#include "../world/chunk_manager.h"
#include "../save/save_slot.h"
#include "../items/inventory.h"
#include "../crafting/crafting.h"
#include "../config/settings.h"
#include "../input/touch.h"
#include <android/asset_manager.h>
#include <functional>
#include <array>
#include <string>

namespace ui {

enum class Screen {
    Hud,
    PauseMenu,
    Inventory,
    Settings,
    SkillTree,
    Attributes,
    Dialogue,
    QuestLog,
    Reputation,
    SaveLoad,
    Crafting,
    Trade,
    Enchant,
};

enum class SaveLoadMode : u8 { Save = 0, Load };
enum class SettingsTab : u8 { Input = 0, Ui, Audio, Game, Render, Count };

struct TradeContext {
    u32 traderEntity = 0;
    u8  tab          = 0;
    i32 selectedIdx  = -1;
    u16 selCount     = 1;
};

struct EnchantContext {
    u32 altarEntity = 0;
    i32 selectedIdx = -1;
};

class UiSystem {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr);
    void destroy();

    void setScreenSize(i32 w, i32 h);

    /// Поворот вывода — тот же, что у камеры.
    void setSurfaceRotation(u32 degrees) { renderer_.setSurfaceRotation(degrees); }

    u32 lastVertices() const { return renderer_.lastVertices(); }
    u32 lastDrawn()    const { return renderer_.lastDrawn(); }

    /// Экранное управление рисуется по состоянию TouchInput: кнопки
    /// и джойстик заведены там, а до сих пор не рисовались нигде —
    /// игрок видел пустой экран и искал кнопки наугад.
    void attachTouch(const input::TouchInput* t) { touch_ = t; }

    bool routeTouch(i32 id, float px, float py, int phase);

    void render(vk::Context& ctx,
                player::Player& player,
                world::ChunkManager& world,
                f32 fps);

    Screen screen = Screen::Hud;
    bool showFps = true;

    /// ---- SaveLoad ----
    SaveLoadMode saveLoadMode = SaveLoadMode::Save;
    std::array<std::array<save::SlotMeta, 3>, 3> slotMeta{};
    void refreshSlotMeta(save::SaveSlotManager& mgr);

    // ---- Crafting ----
    crafting::StationType nearbyStation = crafting::StationType::None;
    i32 selectedRecipeIdx = -1;

    /// ---- Trade ----
    TradeContext tradeCtx{};

    /// ---- Enchant ----
    EnchantContext enchantCtx{};
    u32 nearbyAltar = 0;

    /// ---- Drag & drop ----
    DragDrop drag{};

    /// ---- Миникарта ----
    Minimap minimap;

    /// ---- Настройки ----
    SettingsTab settingsTab = SettingsTab::Input;

    /// ---- Коллбэки ----
    std::function<void(u32 profile, u32 slot)> onSaveRequested;
    std::function<void(u32 profile, u32 slot)> onLoadRequested;
    std::function<void(u32 profile, u32 slot)> onDeleteRequested;
    std::function<void(u32 slotIndex)>         onUseItem;
    std::function<void(u32 slotIndex)>         onDropItem;
    std::function<void(u32 slotIndex, i32 dst)>onMoveItem;
    std::function<void(u16 itemId, u16 count)> onTradeBuy;
    std::function<void(u16 itemId, u16 count)> onTradeSell;
    std::function<void(u32 recipeId)>          onCraft;
    std::function<void(u32 recipeId)>          onEnchant;
    std::function<void(u32 slotIndex)>         onEquipHotbar;
    std::function<void()>                      onSettingsChanged;

    /// ---- Утилиты ----
    void setStatus(const std::string& msg);
    void drawLoadingOverlay();
    /// Джойстик и экранные кнопки. Только поверх чистого HUD: под
    /// открытым меню управление не работает, рисовать его незачем.
    void drawTouchControls();
    void tickUi(f32 dt);

    bool paused() const {
        return screen == Screen::PauseMenu ||
               screen == Screen::Inventory ||
               screen == Screen::Settings ||
               screen == Screen::SkillTree ||
               screen == Screen::Attributes ||
               screen == Screen::QuestLog ||
               screen == Screen::Reputation ||
               screen == Screen::SaveLoad ||
               screen == Screen::Crafting ||
               screen == Screen::Trade ||
               screen == Screen::Enchant;
    }

    bool dialogueOpen() const { return screen == Screen::Dialogue; }

    // ---- Экран загрузки (ТЗ 4.6) ----
    /// Доля готовности мира вокруг игрока, 0..1. Пока меньше единицы,
    /// поверх HUD показывается прогресс-бар: чанки подгружаются
    /// асинхронно, и без индикатора игрок видит пустоту без объяснения.
    f32  loadProgress = 1.f;
    /// Что именно грузится — строка под полосой.
    const char* loadLabel = nullptr;

    bool loading() const { return loadProgress < 0.999f; }

    /// Режим раскладки: кнопки можно перетаскивать по экрану (ТЗ 5.2).
    /// Пока включён, обычные действия кнопок не срабатывают.
    bool buttonLayoutMode = false;

    /// Аппаратная кнопка «Назад»: закрывает текущий экран, а не игру.
    /// Из HUD открывает паузу — так же, как это делают все Android-игры.
    void onBackPressed() {
        switch (screen) {
            case Screen::Hud:
                screen = Screen::PauseMenu;
                break;
            case Screen::PauseMenu:
                screen = Screen::Hud;
                break;
            case Screen::Dialogue:
                // Диалог закрывается своим обработчиком, чтобы NPC
                // вышел из состояния Talk.
                if (onCloseDialogue) onCloseDialogue();
                screen = Screen::Hud;
                break;
            default:
                // Любой вложенный экран возвращает в паузу.
                screen = Screen::PauseMenu;
                break;
        }
    }

    /// Кнопка Start на геймпаде.
    void togglePause() {
        screen = (screen == Screen::Hud) ? Screen::PauseMenu : Screen::Hud;
    }

    /// Вызывается, когда «Назад» закрывает диалог.
    std::function<void()> onCloseDialogue;

    std::function<void()> onSave;
    std::function<void()> onQuit;

    void attachExternalAtlas(VkImageView view, VkSampler sampler) {
        renderer_.attachExternalAtlas(view, sampler);
    }

    void setDialogueActive(bool active) {
        if (active) screen = Screen::Dialogue;
        else if (screen == Screen::Dialogue) screen = Screen::Hud;
    }

    void openInventory() { screen = Screen::Inventory; drag.clear(); }
    void openCrafting(crafting::StationType st) {
        nearbyStation = st;
        selectedRecipeIdx = -1;
        screen = Screen::Crafting;
    }
    void openTrade(u32 traderEntity) {
        tradeCtx.traderEntity = traderEntity;
        tradeCtx.tab = 0;
        tradeCtx.selectedIdx = -1;
        tradeCtx.selCount = 1;
        screen = Screen::Trade;
    }
    void openEnchant(u32 altarEntity) {
        enchantCtx.altarEntity = altarEntity;
        enchantCtx.selectedIdx = -1;
        screen = Screen::Enchant;
    }

private:
    const input::TouchInput* touch_ = nullptr;

    void drawHud(vk::Context& ctx, player::Player& player,
                 world::ChunkManager& world, f32 fps);
    void drawXpBar(player::Player& player);
    void drawResonanceBar(player::Player& player);
    void drawStatusIcons(player::Player& player);
    void drawLevelUpNotification(player::Player& player);
    void drawReputationNotification(player::Player& player);
    void drawMinimap(player::Player& player);
    void drawHudResources(player::Player& player);

    void drawPauseMenu(player::Player& player);
    void drawInventory(player::Player& player);
    void drawHotbar(player::Player& player);
    void drawSkillTreeScreen(player::Player& player);
    void drawAttributesScreen(player::Player& player);

    void drawDialogueScreen(player::Player& player);
    void drawQuestLogScreen(player::Player& player);
    void drawReputationScreen(player::Player& player);

    void drawSaveLoadScreen(player::Player& player);
    void drawCraftingScreen(player::Player& player);
    void drawTradeScreen(player::Player& player);
    void drawEnchantScreen(player::Player& player);
    void drawSettingsScreen(player::Player& player);

    void drawStatusToast();

    /// ---- Помощники ----
    void drawItemIcon(items::ItemStack& stack, float x, float y, float size,
                      bool selected);
    void drawDragOverlay();

    UiRenderer renderer_;
    UiContext  ui_;
    i32        screenW_ = 1080;
    i32        screenH_ = 1920;
    VkDevice   dev_ = VK_NULL_HANDLE;

    /// Скролл
    Scroll craftScroll;
    Scroll questScroll;
    Scroll tradeScroll;

    std::string statusMessage;
    f32         statusTimer = 0.f;

    /// Кэш для HUD (чтобы не дёргать ECS каждый кадр)
    f32 cachedHpPct = 1.f;
    f32 cachedMpPct = 1.f;
    f32 cachedSpPct = 1.f;
};

} // namespace ui
