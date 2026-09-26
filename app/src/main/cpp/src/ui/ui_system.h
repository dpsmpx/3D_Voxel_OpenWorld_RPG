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
#include "preview_atlas.h"
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
    /// Главное меню: с него игра начинается и в него выходят из
    /// паузы. Фоном — тот же мир тем же рендером, камера облетает
    /// место, где стоит игрок.
    MainMenu,
};

/// Разделы меню паузы.
///
/// Список вынесен из drawPauseMenu, потому что это факт об игре, а не
/// деталь рисования: «достижим ли экран из меню» — единственный
/// способ туда попасть, и проверить его иначе не к чему обратиться.
/// Значок с очками считается на месте, при отрисовке: он зависит от
/// игрока, а список — нет.
///
/// В паузе только то, что нужно, пока игра стоит. Частое и
/// привязанное к месту живёт на главном экране: сумка — кнопкой
/// HUD, журнал — блоком текущего задания, ремесло — подсказкой у
/// станка и кнопкой в сумке, миры — в главном меню.
enum class MenuGroup : u8 {
    Character,   ///< развитие персонажа: атрибуты, навыки
    World,       ///< мир вокруг: репутация у фракций
    System,      ///< сохранение и настройки
};
constexpr u32 MENU_GROUPS = 3;

struct MenuEntry {
    config::StrKey label;
    Screen      target;
    MenuGroup   group;
    /// Второстепенный раздел: кнопка ниже и тише остальных.
    bool        secondary = false;
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
/// Раскладка разговора под его содержимое: панель, где имя, где
/// реплика и где каждый ответ.
///
/// В `choices` — только ответы, поместившиеся целиком; проверка
/// спрашивает ровно эту геометрию, по которой экран ловит касания.
struct DialogueLayout {
    Rect panel{0.f, 0.f, 0.f, 0.f};
    f32  textX = 0.f, textW = 0.f;
    f32  nameY = 0.f, textY = 0.f;
    u32  cols  = 1;
    std::vector<Rect> choices;
};

struct NewWorldLayout {
    Rect name{0.f, 0.f, 0.f, 0.f};
    Rect seed{0.f, 0.f, 0.f, 0.f};
    Rect random{0.f, 0.f, 0.f, 0.f};
    Rect create{0.f, 0.f, 0.f, 0.f};
    Rect keyboard{0.f, 0.f, 0.f, 0.f};
};
enum class SettingsTab : u8 { Input = 0, Ui, Audio, Game, Render, Count };

/// Раскладка паузы: где «продолжить», где «в меню», где подпись
/// каждой группы и кнопка каждого раздела.
///
/// Отдельно от рисования, как и остальные раскладки: проверка
/// спрашивает ровно ту геометрию, по которой пауза ловит касания.
/// Прямоугольники — без прокрутки: рисующий сдвигает их на неё сам.
struct PauseLayout {
    Rect area{0.f, 0.f, 0.f, 0.f};     ///< видимая область меню
    Rect resume{0.f, 0.f, 0.f, 0.f};
    Rect toMenu{0.f, 0.f, 0.f, 0.f};
    std::array<Rect, MENU_GROUPS> groupTitle{};
    std::vector<Rect> entries;           ///< по menuEntries()
    /// Группы стоят столбцами рядом; на узком экране — стопкой.
    bool columns = true;
    /// Высота всего содержимого — для прокрутки.
    f32 contentH = 0.f;
};

/// Раскладка главного меню: левая панель с названием и кнопками,
/// справа — открытый мир.
struct MainMenuLayout {
    Rect panel{0.f, 0.f, 0.f, 0.f};      ///< затемнённая полоса слева
    Rect title{0.f, 0.f, 0.f, 0.f};
    /// Продолжить, миры, настройки, выход — сверху вниз.
    static constexpr u32 BUTTONS = 4;
    std::array<Rect, BUTTONS> buttons{};
    /// Строка под «продолжить»: какой мир продолжится.
    Rect caption{0.f, 0.f, 0.f, 0.f};
};

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

/// Подпись над поясом: имя того, что в ВЫБРАННОЙ ячейке, на языке
/// игрока. Пустая ячейка — в дело идёт то, что в руке, и подпись его;
/// пустые и ячейка, и рука — подписи нет (nullptr).
const char* hotbarCaption(const items::Inventory* inv,
                          const combat::EquippedWeapon& hand);

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

    /// Куда складывать всё, что вышло на экран буквами.
    /// См. UiContext::setTextSink. В игре не зовётся.
    void setTextSink(std::vector<std::string>* sink) { ui_.setTextSink(sink); }

    /// Насколько содержимое инвентаря выше отведённой ему области.
    /// Ноль — помещается целиком.
    /// Куда смотрит игрок, в радианах.
    ///
    /// Нужен отметкам урона: они показывают направление ОТНОСИТЕЛЬНО
    /// взгляда, и поворот головы обязан их поворачивать. Yaw живёт в
    /// игровом цикле, а не в игроке, поэтому передаётся сюда явно.
    void setViewYaw(f32 yaw) { viewYaw_ = yaw; }
    f32  viewYaw() const { return viewYaw_; }

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
    /// Отрезки кадра: какие вершины рисуются какой картинкой.
    const std::vector<UiRenderer::Run>& frameRuns() const {
        return renderer_.pendingRuns();
    }
    u32 lastDrawn()    const { return renderer_.lastDrawn(); }
    u32 lastDrawCalls() const { return renderer_.lastDrawCalls(); }

    /// Экранное управление рисуется по состоянию TouchInput: кнопки
    /// и джойстик заведены там, а до сих пор не рисовались нигде —
    /// игрок видел пустой экран и искал кнопки наугад.
    void attachTouch(const input::TouchInput* t) { touch_ = t; }
    /// Пояс быстрых слотов на экране, в пикселях: джойстику туда
    /// нельзя (input::TouchInput::setJoystickKeepOut).
    Rect hotbarArea() const { return layout_.hotbar(); }

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
    /// Его название — главное меню говорит, что именно продолжится.
    std::string currentWorldName;

    /// Превью миров по слотам: где в атласе картинка слота. Атлас
    /// строит и загружает движок (ui/preview_atlas.h), сюда приходит
    /// только разметка.
    std::array<std::array<PreviewCell, 3>, 3> worldPreview{};
    /// Атлас превью миров. Владеет им вызывающий.
    void setWorldPreviewImage(VkImageView view, VkSampler sampler);

    /// Раскладки — ровно те, по которым экраны рисуют и ловят касания.
    PauseLayout pauseLayout() const;
    MainMenuLayout mainMenuLayout() const;
    /// Кнопка ремесла в строке заголовка сумки.
    Rect inventoryCraftButton() const {
        return layout_.titleAction(MODE_BUTTON_W_DP);
    }
    /// «Новый мир» в строке заголовка экрана миров.
    Rect worldsNewButton() const {
        return layout_.titleAction(MODE_BUTTON_W_DP);
    }
    /// Прокрутка паузы — проверке, чтобы докрутить до ряда, как палец.
    Scroll& pauseScrolling() { return pauseScroll; }
    /// Блок текущего задания на HUD — он же вход в журнал. Не ниже
    /// цели касания: блок стал кнопкой.
    Rect questTrackerRect() const {
        Rect r = layout_.questTracker();
        const f32 minH = layout_.dp(theme::TOUCH_MIN_DP);
        if (r.h < minH) r.h = minH;
        return r;
    }

    /// Геометрия экрана создания мира — та же, по которой он рисует.
    NewWorldLayout newWorldLayout() const;
    /// Раскладка разговора: реплика `text`, `choiceCount` ответов, есть
    /// ли строка с именем собеседника.
    DialogueLayout dialogueLayout(const std::string& text, u32 choiceCount,
                                  bool hasName) const;
    SettingsLayout settingsLayout() const;
    /// Прямоугольник вкладки настроек.
    Rect settingsTabRect(u32 index) const;
    /// Ширина кнопки «сбросить всё».
    static constexpr f32 RESET_W_DP = 220.f;

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
    /// Касание ячейки пояса прямо в игре, номер ячейки 0..8.
    std::function<void(u32 hotbarIndex)>       onHotbarTap;
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
    /// Лежит ли под точкой видимая круглая кнопка.
    bool padButtonAt(float px, float py) const;
    /// Джойстик и экранные кнопки. Только поверх чистого HUD: под
    /// открытым меню управление не работает, рисовать его незачем.
    void drawTouchControls();
    void tickUi(f32 dt);

    /// Отнято ли управление: ввод и круглые кнопки гаснут, музыка
    /// стихает. Мир под меню живёт дальше — кроме главного меню, где
    /// игры нет вовсе (см. main.cpp, updateMainMenu).
    ///
    /// Списком «где управление есть», а не «где его нет»: оно есть
    /// только под чистым HUD и под разговором. Список экранов без
    /// управления однажды забыл снимок мира — под ним работали
    /// джойстик и кнопки, которых не видно, — а новый экран, не
    /// вписанный никуда, заберёт управление, а не оставит его под
    /// меню.
    bool paused() const {
        if (atMainMenu) return true;
        return screen != Screen::Hud && screen != Screen::Dialogue;
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
            case Screen::MainMenu:
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

    /// Откуда пришли: экраны под текущим, ближний — последним.
    ///
    /// Раньше «откуда» было одно поле, и цепочка из трёх экранов его
    /// теряла: пауза → миры → новый мир → «закрыть» → «закрыть»
    /// приводило обратно в «новый мир», а настройки, открытые из
    /// главного меню, закрывались бы в игру, которой ещё нет.
    ///
    /// Глубина ограничена: глубже четырёх экранов в игре не бывает,
    /// а при переполнении забывается самый дальний — «назад» всё
    /// равно приведёт на основу (см. baseScreen).
    static constexpr u32 NAV_DEPTH = 6;
    std::array<Screen, NAV_DEPTH> navStack{};
    u32 navDepth = 0;

    /// Игра не идёт: основа навигации — главное меню, а не HUD.
    ///
    /// Отдельно от `screen`, потому что из главного меню открываются
    /// настройки, миры и снимок — те же экраны, что и из паузы, и
    /// куда они закрываются, решает именно это.
    bool atMainMenu = false;

    /// Куда ведёт «назад», когда идти больше некуда.
    Screen baseScreen() const {
        return atMainMenu ? Screen::MainMenu : Screen::Hud;
    }

    /// Откуда открыт текущий экран; основа — если ниоткуда.
    Screen previousScreen() const {
        return navDepth ? navStack[navDepth - 1] : baseScreen();
    }

    /// Открыть экран, запомнив, откуда.
    ///
    /// Экран, уже лежащий в стопке, не кладётся второй раз: стопка
    /// срезается до него. Иначе «новый мир → миры» копил бы
    /// бесконечную цепочку одних и тех же двух экранов.
    void openScreen(Screen s) {
        if (s == screen) return;
        for (u32 i = 0; i < navDepth; ++i) {
            if (navStack[i] == s) { navDepth = i; screen = s; return; }
        }
        if (navDepth == NAV_DEPTH) {
            for (u32 i = 1; i < NAV_DEPTH; ++i) navStack[i - 1] = navStack[i];
            --navDepth;
        }
        navStack[navDepth++] = screen;
        screen = s;
    }

    /// Закрыть экран туда, откуда пришли, — как «Назад».
    void closeToPrevious() {
        screen = previousScreen();
        if (navDepth) --navDepth;
    }

    /// Встать на экран, забыв, откуда пришли: «продолжить», вход в
    /// мир, выход в главное меню, конец разговора.
    void resetTo(Screen s) {
        navDepth = 0;
        screen = s;
    }

    /// Войти в игру: основа — HUD, стопка пуста.
    void enterGame() {
        atMainMenu = false;
        resetTo(Screen::Hud);
    }

    /// Выйти в главное меню.
    void enterMainMenu() {
        atMainMenu = true;
        drag.clear();
        resetTo(Screen::MainMenu);
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
                resetTo(Screen::Hud);
                break;
            case Screen::MainMenu:
                // С корня «назад» уходит из игры — как у любого
                // приложения, но с вопросом: палец промахивается.
                askQuit();
                break;
            case Screen::Dialogue:
                // Диалог закрывается своим обработчиком, чтобы NPC
                // вышел из состояния Talk.
                if (onCloseDialogue) onCloseDialogue();
                resetTo(Screen::Hud);
                break;
            default:
                closeToPrevious();
                break;
        }
    }

    /// Выход из приложения — с вопросом.
    void askQuit();

    /// Кнопка Start на геймпаде.
    void togglePause() {
        // В главном меню игры нет — и ставить на паузу нечего.
        if (atMainMenu) return;
        if (screen == Screen::Hud) openScreen(Screen::PauseMenu);
        else if (screen == Screen::PauseMenu) resetTo(Screen::Hud);
    }

    /// Вызывается, когда «Назад» закрывает диалог.
    std::function<void()> onCloseDialogue;

    std::function<void()> onSave;
    std::function<void()> onQuit;
    /// Главное меню: войти в открытый мир.
    std::function<void()> onContinue;
    /// Пауза: сохранить мир, снять превью и выйти в главное меню.
    std::function<void()> onExitToMenu;

    void setDialogueActive(bool active) {
        if (active) screen = Screen::Dialogue;
        else if (screen == Screen::Dialogue) resetTo(Screen::Hud);
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

    void drawDialogueScreen(player::Player& player,
                            world::ChunkManager& world);
    void drawQuestLogScreen(player::Player& player);
    /// Текущая цель на HUD: что делать прямо сейчас.
    void drawQuestTracker(player::Player& player);
    /// Отметки по краю экрана: откуда игрока бьют.
    void drawHurtMarks(player::Player& player);
    /// Указатель на оставленное там, где игрок пал.
    void drawLostGoldMark(player::Player& player);
    /// Подробности выбранного задания: цель, прогресс, награда.
    void drawQuestDetails(player::Player& player);
    void drawReputationScreen(player::Player& player);

    void drawSaveLoadScreen(player::Player& player);
    void drawWorldsScreen();
    /// Главное меню поверх открытого мира.
    void drawMainMenu();
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
    /// Раскладка пересобирается здесь же: масштаб интерфейса и
    /// зеркало берутся из настроек, а пересобиралась она только при
    /// смене размера экрана — ползунок масштаба не менял ничего до
    /// перезапуска.
    void notifySettingsChanged() {
        rebuildLayout();
        if (onSettingsChanged) onSettingsChanged();
    }

    void drawStatusToast();
    /// Служебная строка HUD: частота кадров, координаты.
    void drawDebugLine(u32 line, const char* text, f32 scale, UiColor c);
    /// Кнопка закрытия одного вида на всю игру.
    void drawCloseButton(std::function<void()> onClose);
    /// Правый край текста в строке заголовка — левее кнопки закрытия.
    f32 titleTextRight() const {
        return layout_.closeButton().x - layout_.dp(theme::SPACE_M_DP);
    }
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
    /// Ширина кнопки режима в строке заголовка экрана сохранений.
    static constexpr f32 MODE_BUTTON_W_DP = 160.f;

    /// Пауза прокручивается целиком: на узком экране группы встают
    /// стопкой и в высоту не помещаются.
    Scroll pauseScroll;
    /// Уже столбца пауза в столбцы не раскладывается.
    static constexpr f32 PAUSE_MIN_COL_DP = 150.f;
    /// Окно превью в карточке мира: ширина к высоте. Снимок области
    /// 64x64 в изометрии чуть шире, чем выше.
    static constexpr f32 WORLD_PREVIEW_ASPECT = 1.35f;
    /// Строка, укороченная до ширины: с «..» на конце, по буквам.
    std::string fitText(const std::string& text, f32 maxW, f32 scale) const;
    /// Главное меню: ширина столбца кнопок, крупность названия,
    /// плотность левой полосы и ширина её спада к миру.
    static constexpr f32 MAIN_MENU_BUTTON_W_DP = 280.f;
    static constexpr f32 MAIN_MENU_TITLE_SCALE = 6.f;
    static constexpr u8  MAIN_MENU_PANEL_ALPHA = 196;
    static constexpr f32 MAIN_MENU_FADE_DP     = 72.f;
    /// Подложка блока задания: заметна как кнопка, но тише полос.
    static constexpr u8 QUEST_BLOCK_ALPHA = 128;

    /// Ряд фракции на экране репутации: название, отношение и
    /// полоса.
    static constexpr f32 REP_ROW_H_DP = 64.f;

    /// Ширина подписи «очки навыков» в строке заголовка.
    static constexpr f32 POINTS_LABEL_W_DP = 200.f;

    /// Столбцы древа навыков. Восемь узлов в столбце не
    /// помещаются в высоту бюджетного экрана.
    Scroll skillScroll;

    Scroll craftScroll;
    /// Карточки сохранений. Девять штук в полный рост на
    /// бюджетный экран не помещаются.
    Scroll saveScroll;
    /// Список рецептов алтаря. Он длиннее экрана: шестнадцать
    /// ступеней, а помещается пять-шесть.
    Scroll enchantScroll;
    Scroll questScroll;
    Scroll tradeScroll;
    /// Сумка, экипировка и пояс не помещаются на узкий экран все
    /// сразу: 27 + 6 + 9 ячеек требуют около 320 точек при 232
    /// доступных. Раскладкой это не решается — только прокруткой.
    Scroll invScroll;

    /// Куда смотрит игрок. Выставляет игровой цикл каждым кадром.
    f32 viewYaw_ = 0.f;

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
