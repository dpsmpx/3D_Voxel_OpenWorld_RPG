/**
 * @file ui_system.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "ui_renderer.h"
#include "ui_context.h"
#include "drag_drop.h"
#include "scroll.h"
#include "hud_layout.h"
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
#include <vector>
#include <utility>

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

    /// Плотность экрана из AConfiguration_getDensity.
    ///
    /// До неё размеры считались от числа пикселей, и цель касания
    /// выходила 20..36 dp при норме 48: кнопка меню — 3.2 мм при
    /// подушечке пальца 8..10 мм.
    void setDensityDpi(i32 dpi);

    /// Единственный источник геометрии: и отрисовка, и касание.
    const HudLayout& layout() const { return layout_; }

    /// Цвет слоя HUD с учётом настройки прозрачности.
    ///
    /// Слайдер `uiOpacity` двигался и сохранялся, но не читался
    /// нигде — ровно то, что §10 задания запрещает оставлять. Он
    /// про HUD поверх мира: меню остаются непрозрачными, иначе
    /// текст поверх движущейся сцены не прочесть.
    UiColor hudTint(UiColor c) const {
        const u32 a = c & 0xFFu;
        const f32 k = config::settingsConst().uiOpacity;
        const f32 v = (f32)a * (k < 0.f ? 0.f : (k > 1.f ? 1.f : k));
        return withAlpha(c, (u8)(v + 0.5f));
    }

    /// Поворот вывода — тот же, что у камеры.
    void setSurfaceRotation(u32 degrees) { renderer_.setSurfaceRotation(degrees); }

    u32 lastVertices() const { return renderer_.lastVertices(); }
    u32 lastDrawn()    const { return renderer_.lastDrawn(); }
    u32 lastDrawCalls() const { return renderer_.lastDrawCalls(); }

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

    /// Выбранное задание в журнале, -1 — ничего.
    i32 selectedQuest = -1;

    /// Выбранная ячейка инвентаря, -1 — ничего.
    ///
    /// Раньше тап по предмету ОДНОВРЕМЕННО использовал его и начинал
    /// перенос: зелье выпивалось и бралось в руку одним касанием.
    /// Теперь тап только выбирает, а действия — кнопками справа.
    i32 selectedInvSlot = -1;

    /// ---- Drag & drop ----
    DragDrop drag{};

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

    /// Подтверждение необратимого действия.
    ///
    /// Выход из игры срабатывал сразу, без вопроса: несохранённый
    /// прогресс терялся молча. То же относится к удалению и
    /// перезаписи сохранения.
    struct Confirm {
        bool active = false;
        const char* question = nullptr;
        const char* yesLabel = nullptr;
        std::function<void()> onYes;
    };
    Confirm confirm{};

    void askConfirm(const char* question, const char* yesLabel,
                    std::function<void()> onYes) {
        confirm.active = true;
        confirm.question = question;
        confirm.yesLabel = yesLabel;
        confirm.onYes = std::move(onYes);
    }

    // ============================================================
    // Уведомления
    // ============================================================
    //
    // Слот был ОДИН: новое сообщение затирало предыдущее. «Предмет
    // получен» стирало «задание выполнено», и различить важное от
    // рядового было нечем — вид у всех один.
    struct Notice {
        std::string          text;
        theme::NotifyPriority priority = theme::NotifyPriority::Normal;
        f32                  timeLeft = 0.f;
        f32                  age      = 0.f;
    };

    /// Показать уведомление. Новое встаёт в очередь, а не затирает.
    void notify(const std::string& text,
                theme::NotifyPriority p = theme::NotifyPriority::Normal);

    const std::vector<Notice>& notices() const { return notices_; }

    /// ---- Утилиты ----
    /// Прежнее имя: рядовое уведомление.
    void setStatus(const std::string& msg) {
        notify(msg, theme::NotifyPriority::Normal);
    }
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

    /// Куда вернуться из текущего экрана.
    ///
    /// Раньше любой вложенный экран возвращал в паузу, даже если
    /// открыт был из HUD: игрок оказывался не там, откуда пришёл.
    Screen returnTo = Screen::Hud;

    /// Открыть экран, запомнив, откуда.
    void openScreen(Screen s) {
        if (s != screen) returnTo = screen;
        screen = s;
    }

    /// Аппаратная кнопка «Назад»: закрывает текущий экран, а не игру.
    /// Из HUD открывает паузу — так же, как это делают все Android-игры.
    void onBackPressed() {
        // Открытое подтверждение «Назад» отменяет — и только его.
        if (confirm.active) { confirm = Confirm{}; return; }

        switch (screen) {
            case Screen::Hud:
                openScreen(Screen::PauseMenu);
                break;
            case Screen::PauseMenu:
                screen = Screen::Hud;
                returnTo = Screen::Hud;
                break;
            case Screen::Dialogue:
                // Диалог закрывается своим обработчиком, чтобы NPC
                // вышел из состояния Talk.
                if (onCloseDialogue) onCloseDialogue();
                screen = Screen::Hud;
                returnTo = Screen::Hud;
                break;
            default:
                screen = returnTo;
                returnTo = Screen::Hud;
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

    void setDialogueActive(bool active) {
        if (active) screen = Screen::Dialogue;
        else if (screen == Screen::Dialogue) screen = Screen::Hud;
    }

    void openInventory() { openScreen(Screen::Inventory); drag.clear(); }
    void openCrafting(crafting::StationType st) {
        nearbyStation = st;
        selectedRecipeIdx = -1;
        openScreen(Screen::Crafting);
    }
    void openTrade(u32 traderEntity) {
        tradeCtx.traderEntity = traderEntity;
        tradeCtx.tab = 0;
        tradeCtx.selectedIdx = -1;
        tradeCtx.selCount = 1;
        openScreen(Screen::Trade);
    }
    void openEnchant(u32 altarEntity) {
        enchantCtx.altarEntity = altarEntity;
        enchantCtx.selectedIdx = -1;
        openScreen(Screen::Enchant);
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
    /// Подсказка «использовать»: отпирает ремесло и зачарование.
    void drawInteractPrompt();
    void drawHudResources(player::Player& player);

    void drawPauseMenu(player::Player& player);
    void drawInventory(player::Player& player);
    void drawHotbar(player::Player& player);
    /// Одна сетка ячеек: сумка, экипировка и пояс рисуются ею же.
    void drawSlotGrid(player::Player& player, const HudLayout::CellGrid& g,
                      u32 firstSlot, u32 count);
    /// Что за предмет и что с ним можно сделать.
    void drawItemDetails(player::Player& player);
    void drawSkillTreeScreen(player::Player& player);
    void drawAttributesScreen(player::Player& player);
    /// Кнопка «+»/«−» одного вида на всю игру.
    void drawStepper(const Rect& r, const char* label,
                     bool pressed, bool enabled);

    void drawDialogueScreen(player::Player& player);
    void drawQuestLogScreen(player::Player& player);
    /// Текущая цель на HUD: что делать прямо сейчас.
    void drawQuestTracker(player::Player& player);
    /// Подробности выбранного задания: цель, прогресс, награда.
    void drawQuestDetails(player::Player& player);
    void drawReputationScreen(player::Player& player);

    void drawSaveLoadScreen(player::Player& player);
    void drawCraftingScreen(player::Player& player);
    void drawTradeScreen(player::Player& player);
    void drawEnchantScreen(player::Player& player);
    void drawSettingsScreen(player::Player& player);

    void drawStatusToast();
    /// Кнопка закрытия одного вида на всю игру.
    void drawCloseButton(std::function<void()> onClose);
    /// Модальное подтверждение поверх всего.
    void drawConfirm();

    /// ---- Помощники ----
    void drawItemIcon(items::ItemStack& stack, float x, float y, float size,
                      bool selected);
    void drawDragOverlay();

    UiRenderer renderer_;
    UiContext  ui_;
    HudLayout  layout_;
    i32        screenW_ = 1080;
    i32        screenH_ = 1920;
    i32        densityDpi_ = 0;      ///< 0 — система не сообщила
    void       rebuildLayout();
    VkDevice   dev_ = VK_NULL_HANDLE;

    /// Скролл
    Scroll craftScroll;
    Scroll questScroll;
    Scroll tradeScroll;

    /// Очередь уведомлений. Показывается не больше
    /// theme::NOTIFY_MAX_VISIBLE сразу; важное вытесняет рядовое, а
    /// не наоборот.
    std::vector<Notice> notices_;

    /// Кэш для HUD (чтобы не дёргать ECS каждый кадр)
    f32 cachedHpPct = 1.f;
    f32 cachedMpPct = 1.f;
    f32 cachedSpPct = 1.f;
};

} // namespace ui
