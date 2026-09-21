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
#include "text_entry.h"
#include "hud_layout.h"
#include "../player/player.h"
#include "../world/chunk_manager.h"
#include "../save/save_slot.h"
#include "../items/inventory.h"
#include "../crafting/crafting.h"
#include "../combat/focus.h"
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
    /// Список миров: открыть, выгрузить файлом, удалить.
    Worlds,
    /// Создание мира: название, зерно, и что из этого выйдет.
    NewWorld,
    /// Изометрический снимок мира: область, сторона, предпросмотр.
    IsoSnapshot,
};

/// Разделы меню паузы.
///
/// Список вынесен из drawPauseMenu, потому что это факт об игре, а не
/// деталь рисования: «достижим ли экран из меню» — единственный
/// способ туда попасть, и проверить его иначе не к чему обратиться.
/// Значок с очками считается на месте, при отрисовке: он зависит от
/// игрока, а список — нет.
struct MenuEntry {
    config::StrKey label;
    Screen      target;
};
const std::vector<MenuEntry>& menuEntries();

enum class SaveLoadMode : u8 { Save = 0, Load };

/// Какое из двух полей на экране создания мира сейчас набирают.
enum class WorldField : u8 { Name = 0, Seed };

/// Раскладка экрана создания мира.
///
/// Отдельно от рисования по той же причине, что и раскладка
/// клавиатуры: проверке нужна ровно та геометрия, по которой экран
/// ловит касания, а не её копия.
struct NewWorldLayout {
    Rect name{0.f, 0.f, 0.f, 0.f};
    Rect seed{0.f, 0.f, 0.f, 0.f};
    Rect random{0.f, 0.f, 0.f, 0.f};
    Rect create{0.f, 0.f, 0.f, 0.f};
    Rect keyboard{0.f, 0.f, 0.f, 0.f};
};
enum class SettingsTab : u8 { Input = 0, Ui, Audio, Game, Render, Count };

/// Раскладка экрана настроек.
///
/// Вынесена из рисования по той же причине, что и раскладка создания
/// мира: проверке нужна ровно та геометрия, по которой экран ловит
/// касания. Копия правил проверяла бы копию, а расходятся они молча —
/// палец попадает мимо тумблера, и настройка не переключается.
struct SettingsLayout {
    Rect panel{0.f, 0.f, 0.f, 0.f};
    f32  rowH   = 0.f;
    f32  rowGap = 0.f;
    f32  pad    = 0.f;
    f32  colW   = 0.f;
    f32  colGap = 0.f;
    u32  perCol = 1;
    Rect resetAll{0.f, 0.f, 0.f, 0.f};

    /// Строки укладываются по колонкам, а не одним столбцом: вкладка
    /// «Управление» в один столбец не помещается на экран, а прокрутки
    /// у настроек нет.
    Rect row(u32 i) const {
        const u32 col = perCol ? (i / perCol) : 0u;
        const u32 r   = perCol ? (i % perCol) : i;
        return { panel.x + pad + (f32)col * (colW + colGap),
                 panel.y + pad + (f32)r * (rowH + rowGap), colW, rowH };
    }
};

/// Прямоугольник вкладки настроек.
Rect settingsTabRect(u32 index);
/// Сколько строк рисует вкладка. buttonLayout — включён ли режим
/// перемещения кнопок: он добавляет к «Управлению» подсказку и кнопку
/// сброса раскладки.
u32 settingsRowCount(SettingsTab tab, bool buttonLayout);

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

    /// Насколько содержимое инвентаря выше отведённой ему области.
    /// Ноль — помещается целиком.
    f32 inventoryScrollMax() const { return invScroll.maxOffset; }
    f32 inventoryScrollOffset() const { return invScroll.offset; }
    f32 questScrollMax() const { return questScroll.maxOffset; }
    f32 questScrollOffset() const { return questScroll.offset; }
    f32 tradeScrollMax() const { return tradeScroll.maxOffset; }
    void scrollQuestsTo(f32 o) {
        questScroll.offset = o; questScroll.velocity = 0.f;
        questScroll.clampOffset();
    }

    /// Прокрутить инвентарь на заданное смещение. Больше края не
    /// уедет: `Scroll` ограничивает сам.
    void scrollInventoryTo(f32 offset) {
        invScroll.offset = offset;
        invScroll.velocity = 0.f;   // поставить — значит остановить
        invScroll.clampOffset();
    }

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

    /// Треугольники собранного кадра.
    ///
    /// Кадр строится целиком на процессоре (см. buildFrame), но
    /// посмотреть на него до сих пор было нельзя: наружу торчало
    /// только ЧИСЛО вершин. А интерфейс — единственная часть игры,
    /// которую проверки щупают насквозь и при этом ни разу не видели.
    ///
    /// Отсюда их берёт `tools/uishot` и рисует настоящую картинку
    /// программным растеризатором, без Vulkan и без устройства.
    const std::vector<UiVertex>& frameVertices() const {
        return renderer_.pendingVertices();
    }
    u32 lastDrawn()    const { return renderer_.lastDrawn(); }
    u32 lastDrawCalls() const { return renderer_.lastDrawCalls(); }

    /// Экранное управление рисуется по состоянию TouchInput: кнопки
    /// и джойстик заведены там, а до сих пор не рисовались нигде —
    /// игрок видел пустой экран и искал кнопки наугад.
    void attachTouch(const input::TouchInput* t) { touch_ = t; }

    bool routeTouch(i32 id, float px, float py, int phase);

    /// Построить кадр: все вершины интерфейса и все области касания.
    ///
    /// Вулкан здесь не участвует ни разу — интерфейс целиком строится
    /// на процессоре. Поэтому кадр можно собрать и разобрать без
    /// устройства, чем и занимаются проверки в tools/hostcheck: до
    /// сих пор единственный вход был через render(), а он требовал
    /// контекст, которого на хосте нет.
    void buildFrame(player::Player& player,
                    world::ChunkManager& world,
                    f32 fps);

    /// Построить кадр и отправить его на видеокарту.
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

    /// ---- Полоса цели ----
    ///
    /// Кладётся сюда снаружи, раз в кадр, — тем же порядком, что и
    /// `nearbyStation`. Интерфейс не спрашивает реестр сам: ему не
    /// положено знать ни о мобах, ни о фазах босса, ни о том, кто
    /// кого ударил. Ему положено это НАРИСОВАТЬ.
    combat::FocusView target{};

    /// Выбранное задание в журнале, -1 — ничего.
    i32 selectedQuest = -1;

    /// Выбранная ячейка инвентаря, -1 — ничего.
    ///
    /// Раньше тап по предмету ОДНОВРЕМЕННО использовал его и начинал
    /// перенос: зелье выпивалось и бралось в руку одним касанием.
    /// Теперь тап только выбирает, а действия — кнопками справа.
    i32 selectedInvSlot = -1;

    /// ---- Drag & drop ----
    ///
    /// Перенос предмета пальцем. Тап по-прежнему ВЫБИРАЕТ: перенос
    /// начинается только после того, как палец сдвинулся дальше
    /// порога или простоял на месте дольше долгого тапа. Иначе одно
    /// касание значило бы сразу два действия — ровно то, от чего
    /// инвентарь в своё время и уходил.
    ///
    /// Предмет НЕ вынимается из сумки на время переноса: в drag
    /// лежит только «что несём и откуда», а сама перекладка
    /// происходит одним движением при отпускании. Вынимать было бы
    /// проще, но тогда любой выход из экрана посреди переноса — а
    /// его делает drag.clear() — терял бы предмет.
    DragDrop drag{};

    /// ---- Настройки ----
    SettingsTab settingsTab = SettingsTab::Input;

    /// ---- Создание мира ----
    ///
    /// Два поля и клавиатура живут в интерфейсе, а не в движке:
    /// набор — это про экран. Наружу уходит один раз и уже готовым:
    /// имя и то, что игрок набрал в поле зерна.
    WorldField newWorldField = WorldField::Name;
    KeyPage    keyPage       = KeyPage::Cyrillic;
    TextEntry  newWorldName;
    TextEntry  newWorldSeed;

    /// Зерно мира, в котором игрок сейчас: в списке он помечен.
    u64 currentWorldSeed = 0;

    /// Геометрия экрана создания мира — та же, по которой он рисует.
    NewWorldLayout newWorldLayout() const;
    SettingsLayout settingsLayout() const;

    /// Поле, которое сейчас набирают.
    TextEntry& activeField() {
        return newWorldField == WorldField::Name ? newWorldName : newWorldSeed;
    }

    /// Подготовить экран создания: пустое имя и пустое зерно.
    void openNewWorld() {
        newWorldName.clear();
        newWorldSeed.clear();
        newWorldField = WorldField::Name;
        openScreen(Screen::NewWorld);
    }

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
    /// Создать мир. seedText — то, что игрок НАБРАЛ, а не число:
    /// «12345» и «Долина» разбираются одинаково и в одном месте,
    /// а интерфейсу знать это правило незачем.
    std::function<void(const char* name, const char* seedText)> onCreateWorld;
    /// Выгрузить мир одним файлом наружу и забрать такой файл обратно.
    std::function<void(u32 profile, u32 slot)> onExportRequested;
    std::function<void(u32 profile, u32 slot)> onImportRequested;

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
               screen == Screen::Enchant ||
               screen == Screen::Worlds ||
               screen == Screen::NewWorld;
    }

    bool dialogueOpen() const { return screen == Screen::Dialogue; }

    /// Виден ли HUD под этим экраном.
    ///
    /// Один список на два вопроса: звать ли `drawHud` и обходить ли
    /// раскладке столбец ресурсов. Пока ответов было два, они
    /// расходились — экран создания мира отступал от столбца,
    /// которого под ним нет.
    ///
    /// Под остальными HUD рисуется намеренно: `paused()` гасит
    /// только ввод и музыку, мир продолжает жить, и полоса здоровья
    /// нужна игроку ровно тогда, когда он копается в сумке.
    static bool hudVisibleUnder(Screen s) {
        switch (s) {
            case Screen::Settings:
            case Screen::Worlds:
            case Screen::NewWorld:
            case Screen::IsoSnapshot:
                return false;
            default:
                return true;
        }
    }

    // ---- Экран загрузки (ТЗ 4.6) ----
    /// Доля готовности мира вокруг игрока, 0..1. Пока меньше единицы,
    /// поверх HUD показывается прогресс-бар: чанки подгружаются
    /// асинхронно, и без индикатора игрок видит пустоту без объяснения.
    f32  loadProgress = 1.f;
    /// Что именно грузится — строка под полосой.
    const char* loadLabel = nullptr;

    bool loading() const { return loadProgress < 0.999f; }

    // ---- Изометрический снимок мира ----
    //
    // Интерфейс не владеет ни Vulkan, ни миром: он показывает
    // состояние и зовёт обратно. Ровно так же устроены торговля,
    // зачарование и создание мира.
    struct IsoUi {
        /// Что выбрал игрок.
        i32 size = 100;            ///< сторона области в БЛОКАХ
        u32 view = 0;              ///< 0 север, 1 восток, 2 юг, 3 запад

        /// Что происходит. Значения совпадают с render::IsoStage.
        u8  stage = 0;             ///< 0 Idle, 1 Preparing, 2 Meshing,
                                   ///< 3 Rendering, 4 Ready, 5 Failed
        f32 progress = 0.f;
        /// Совпадает с render::IsoError.
        u8  error = 0;
        /// Идёт финальный снимок (а не предпросмотр).
        bool capturing = false;

        /// Центр области — блок, в котором стоит игрок.
        i32 centerX = 0, centerZ = 0;
        /// Размер готовой картинки, пикселей.
        u32 outW = 0, outH = 0;
        /// Куда сохранили. Пусто — ещё не сохраняли.
        std::string savedPath;
    };
    IsoUi iso;

    /// Игрок сменил размер или сторону: пересобрать предпросмотр.
    std::function<void()> onIsoParamsChanged;
    /// «Готово»: снять в полном разрешении и сохранить.
    std::function<void()> onIsoCapture;
    /// «Отмена» или уход с экрана.
    std::function<void()> onIsoCancel;

    /// Картинка предпросмотра. Владеет ею вызывающий: интерфейс
    /// только ссылается на неё, пока она показана.
    void setPreviewImage(VkImageView view, VkSampler sampler);

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

    void drawHud(player::Player& player,
                 world::ChunkManager& world, f32 fps);
    void drawXpBar(player::Player& player);
    void drawTargetBar();
    void drawResonanceBar(player::Player& player);
    void drawStatusIcons(player::Player& player);
    void drawLevelUpNotification(player::Player& player);
    void drawReputationNotification(player::Player& player);
    /// Подсказка «использовать»: отпирает ремесло и зачарование.
    void drawInteractPrompt();
    void drawHudResources(player::Player& player);

    /// Фон и заголовок полноэкранного экрана — одним вызовом.
    ///
    /// Единственное место, где они появляются. До этого каждый экран
    /// рисовал их сам, и семь из десяти — сырыми числами мимо темы и
    /// раскладки: `rect(0, 0, screenW_, screenH_, rgba(10, 5, 30,
    /// 235))` и заголовок по доле ширины. Отсюда и разнобой фонов, и
    /// заголовки, лежащие на полосах HUD.
    void drawMenuBackdrop(const char* title);

    void drawPauseMenu(player::Player& player);
    void drawInventory(player::Player& player);
    void drawHotbar(player::Player& player);
    /// Одна сетка ячеек: сумка, экипировка и пояс рисуются ею же.
    ///
    /// `clip` — что из неё видно: ячейки целиком вне этого
    /// прямоугольника не рисуются и касаний не ловят. Пустой
    /// прямоугольник (нулевой высоты) означает «видно всё».
    void drawSlotGrid(player::Player& player, const HudLayout::CellGrid& g,
                      u32 firstSlot, u32 count, Rect clip = {});

    /// Прокрутка пальцем внутри области.
    ///
    /// `Scroll` умеет `beginDrag`/`updateDrag`/`endDrag` с самого
    /// своего появления — и не звал их НИКТО: списки ремесла,
    /// журнала и торговли листались одними кнопками «вверх» и
    /// «вниз». Написанный и неподключённый жест — ровно та болезнь,
    /// которую в этом проекте уже лечили у квестовых уведомлений и
    /// у эликсиров.
    ///
    /// `blocked` — не начинать прокрутку с этого касания: в
    /// инвентаре палец, опущенный на занятую ячейку, несёт предмет,
    /// а не листает. Начатую прокрутку это не прерывает.
    void feedScrollDrag(Scroll& sc, const Rect& area, bool blocked = false);
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
    void drawWorldsScreen();
    void drawNewWorldScreen();
    void drawIsoSnapshotScreen();
    /// Экранная клавиатура. Область — то, что от экрана осталось.
    void drawKeyboard(Rect area);
    void drawCraftingScreen(player::Player& player);
    void drawTradeScreen(player::Player& player);
    void drawEnchantScreen(player::Player& player);
    void drawSettingsScreen(player::Player& player);
    /// Сказать игре, что настройки поменялись.
    ///
    /// Раньше это был лямбда-объект на стеке drawSettingsScreen, и
    /// обработчики тумблеров захватывали его ПО ССЫЛКЕ. Но обработчик
    /// живёт дольше кадра: UiContext копирует его при нажатии и
    /// вызывает при отпускании — через пять-семь кадров, когда кадр,
    /// создавший лямбду, давно свёрнут. Нажатие на любой тумблер
    /// читало мёртвый стек и роняло игру. Метод класса живёт столько
    /// же, сколько сам UiSystem, и захватывать его не нужно вовсе.
    void notifySettingsChanged() { if (onSettingsChanged) onSettingsChanged(); }

    void drawStatusToast();
    /// Служебная строка HUD: частота кадров, координаты.
    void drawDebugLine(u32 line, const char* text, f32 scale, UiColor c);
    /// Кнопка закрытия одного вида на всю игру.
    void drawCloseButton(std::function<void()> onClose);
    /// Модальное подтверждение поверх всего.
    void drawConfirm();

    /// ---- Помощники ----
    void drawItemIcon(items::ItemStack& stack, float x, float y, float size,
                      bool selected);
    void drawDragOverlay();

    /// ---- Перенос предмета пальцем ----
    /// Замечает начало переноса и завершает его при отпускании.
    /// Зовётся из экрана инвентаря, после всех сеток ячеек.
    void updateSlotDrag(items::Inventory& inv);

    /// Ячейка, на которой палец задержался перед началом переноса.
    i32       dragPressSlot_ = -1;
    glm::vec2 dragPressPos_{0.f, 0.f};
    f32       dragPressTime_ = 0.f;
    /// Был ли палец на экране в прошлом кадре: отпускание — это
    /// переход true → false, отдельного события интерфейс не даёт.
    bool      dragPointerWas_ = false;
    /// Ячейка под пальцем в момент отпускания; -1, если мимо всех.
    i32       dragDropSlot_ = -1;
    /// Шаг времени последнего кадра — для порога долгого тапа.
    f32       uiDt_ = 0.f;

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
    /// Сумка, экипировка и пояс не помещаются на узкий экран все
    /// сразу: 27 + 6 + 9 ячеек требуют около 320 точек при 232
    /// доступных. Раскладкой это не решается — только прокруткой.
    Scroll invScroll;

    /// Очередь уведомлений. Показывается не больше
    /// theme::NOTIFY_MAX_VISIBLE сразу; важное вытесняет рядовое, а
    /// не наоборот.
    std::vector<Notice> notices_;

    /// Кэш для HUD (чтобы не дёргать ECS каждый кадр)
    f32 cachedHpPct = 1.f;
    f32 cachedMpPct = 1.f;
    f32 cachedSpPct = 1.f;
    /// Потолок выносливости: утомление отрезает хвост полоски.
    f32 cachedSpCeil = 1.f;
    /// Воздух. Полоска показывается только когда он убывает.
    f32 cachedAirPct = 1.f;
};

} // namespace ui
