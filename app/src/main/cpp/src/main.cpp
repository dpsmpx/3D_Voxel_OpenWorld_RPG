// ============================================================
// src/main.cpp — точка входа VoxelRPG. Фазы 1–15.
// ============================================================

#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>

#include <chrono>
#include <memory>
#include <vector>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>
#include <ctime>
#include <string>
#include <algorithm>
#include <atomic>

#include "core/clipboard.h"
#include "core/file_picker.h"
#include "core/log.h"
#include "core/job_system.h"
#include "core/math.h"
#include "core/orientation.h"
#include "entity/locomotion.h"

#include "config/settings.h"
#include "config/localization.h"
#include "config/playtime.h"

#include "input/touch.h"
#include "input/touch_layout.h"

#include "vk/vk_context.h"

#include "world/chunk_manager.h"
#include "world/debug_scene.h"
#include "world/block.h"
#include "world/enchant_altar.h"
#include "world/day_cycle.h"

#include "render/render_system.h"

#include "player/player.h"
#include "physics/raycast.h"
#include "physics/collision.h"

#include "mobs/spawner.h"
#include "world/hazards.h"
#include "world/spawn.h"
#include "world/weather.h"
#include "world/precipitation.h"
#include "world/world_spec.h"
#include "save/save_export.h"
#include "mobs/mob_ai.h"

#include "combat/projectile.h"
#include "combat/status_effects.h"
#include "combat/weapon.h"
#include "combat/spatial_hash.h"

#include "progression/progression.h"

#include "quests/quest.h"
#include "factions/faction.h"
#include "npc/npc_spawner.h"
#include "npc/npc_ai.h"
#include "npc/dialogue.h"

#include "items/item_pickup.h"
#include "items/throwable.h"
#include "items/item_use.h"
#include "items/inventory.h"
#include "items/currency.h"

#include "crafting/crafting.h"
#include "crafting/crafting_station.h"
#include "crafting/recipe.h"

#include "trade/trade.h"

#include "audio/audio_engine.h"
#include "audio/music.h"
#include "audio/audio_events.h"

#include "save/save_manager.h"
#include "save/save_inventory.h"

#include "ui/ui_system.h"

#include "ecs/registry.h"
#include "ecs/components.h"

// Музыкальный трек места вычисляется сложением: номер биома или
// уклада прибавляется к номеру первого трека блока. Значит, порядок
// в audio::SoundId обязан совпадать с порядком в мире — и это
// единственное место, где о такой связи вообще можно узнать.
static_assert((u32)world::BIOME_COUNT == audio::MUSIC_BIOME_COUNT,
              "у каждого биома должен быть свой трек");
static_assert((u32)world::VillageStyle::Count == audio::MUSIC_VILLAGE_COUNT,
              "у каждого уклада деревни должен быть свой трек");
static_assert((u32)world::Ocean  == 0u && (u32)world::Blight  == 11u,
              "порядок биомов в BiomeId сменился — переберите таблицу музыки");
static_assert((u32)world::VillageStyle::Farmstead == 0u &&
              (u32)world::VillageStyle::Woodland  == 3u,
              "порядок укладов сменился — переберите таблицу музыки");

// Отпечаток сборки: заголовок пишется на КАЖДУЮ сборку (см.
// cmake/build_sha.cmake). Через __has_include — чтобы хостовые
// инструменты, собирающие этот файл без CMake, не падали.
#if defined(__has_include)
#  if __has_include("build_sha.h")
#    include "build_sha.h"
#  endif
#endif

using namespace ecs;
namespace cfg = config;


struct Engine {
    vk::Context                            vk;
    input::TouchInput                      touch;
    ecs::Registry                          registry;
    std::unique_ptr<world::ChunkManager>   world;
    std::unique_ptr<render::RenderSystem>  render;
    std::unique_ptr<player::Player>        player;
    std::unique_ptr<ui::UiSystem>          ui;
    std::unique_ptr<mobs::Spawner>         spawner;
    std::unique_ptr<hazards::TrapSpawner>  trapSpawner;
    /// Вскрытые тайники живут рядом с сохранением, а не в спавнере:
    /// это то, что в мире УЖЕ случилось.
    hazards::TreasureKeeper                treasures;
    std::unique_ptr<npc::NpcSpawner>       npcSpawner;
    std::unique_ptr<crafting::StationSpawner>  stationSpawner;
    std::unique_ptr<world::EnchantAltarSpawner> altarSpawner;

    save::SaveManager                      saveMgr;
    save::WorldDeltaStore                  worldDelta;

    // Phase 14/15: audio
    audio::MusicDirector                   musicDirector;
    audio::MusicContext                    musicCtx;
    /// Биом под игроком, обновляется вместе с остальной обстановкой.
    u32                                    cachedBiome_ = (u32)world::Plains;
    f32                                    footstepTimer = 0.f;

    // Phase 15: spatial hash
    combat::SpatialHash                    spatialHash;

    /// Зерно текущего мира. Настоящее значение берётся при входе в
    /// мир: до него мира нет, а ноль — честное «никакого».
    u64  worldSeed      = 0;

    /// Игровые сутки: спавн мобов, солнце, цвет неба, ассортимент
    /// торговцев. Сохраняются вместе с миром.
    world::DayCycle                        dayCycle;

    /// Погода: циклоны, тучи, осадки, радуга.
    ///
    /// Отдельно сохранять её нечего: она выводится из зерна мира и
    /// времени, а то и другое сейв уже помнит.
    world::Weather                         weather;

    /// Капли и снежинки вокруг игрока.
    world::Precipitation                   precip;
    /// Их же в виде инстансов — буфер живёт между кадрами, чтобы не
    /// выделять память шестьдесят раз в секунду.
    std::vector<render::MobInstance>       precipInstances;

    cfg::PlaytimeTracker playtime;
    f32  autosaveTimer  = 0.f;

    u32  lastSaveProfile = 0;
    u32  lastSaveSlot    = 0;

    std::string settingsPath;
    /// Нужен для JNI: буфер обмена живёт на стороне Java, а
    /// добраться до него можно только через объект активности.
    ANativeActivity* activity = nullptr;
    std::string internalDataPath;
    /// Общее хранилище: оттуда выгруженный мир видят другие
    /// программы. Система оставляет путь пустым, если хранилище на
    /// момент запуска не смонтировано, — это нормально, каталог
    /// подбирается и без него.
    std::string externalDataPath;

    /// Как игрок назвал текущий мир. Попадает в метаданные слота и
    /// в имя выгруженного файла.
    char worldName[world::WORLD_NAME_CAP] = {};

    /// Какой из выгруженных файлов возьмёт запасной путь загрузки.
    ///
    /// Нужен только там, где системного диалога нет вовсе.
    u32 importPick = 0;

    /// В какой слот ляжет мир, который сейчас выбирают.
    struct ImportTarget { u32 profile = 0; u32 slot = 0; };
    ImportTarget importInto{};

    /// Профиль слота, который продолжаем при запуске; -1 — начинаем
    /// новый мир. Сам слот берётся из настроек — здесь только
    /// признак, что продолжать есть что.
    i32 continueSlot = -1;

    glm::vec2 cameraYawPitch{ 0.f, -0.35f };
    /// Счётчики отладочной сцены — только для доказательства изоляции.
    f32 debugSceneTimer_  = 0.f;
    u64 debugSceneFrames_ = 0;
    glm::vec3 playerSpawn{ 4.5f, 60.f, 4.5f };

    u32 btnJump_    = 0;
    u32 btnDash_    = 0;
    u32 btnBreak_   = 0;
    u32 btnPlace_   = 0;
    u32 btnCamera_  = 0;
    u32 btnAttack_  = 0;
    u32 btnBlock_   = 0;
    u32 btnFinisher_= 0;
    u32 btnInteract_= 0;
    u32 btnUseItem_ = 0;

    /// id кнопок по слотам настроек — для применения раскладки.
    u32 buttonIds_[cfg::Settings::BUTTON_SLOTS] = {};

    bool evJump_    = false;
    bool evBreak_   = false;
    bool evPlace_   = false;
    bool evAttack_  = false;
    bool evFinish_  = false;
    bool evInteract_= false;
    bool evUseItem_ = false;
    bool evDash_    = false;
    bool evBlock_   = false;

    bool running     = false;
    bool initialized = false;

    /// Мир под игроком ещё генерируется — физику держим выключенной.
    bool waitingForGround = false;

    // ------------------------------------------------------------
    // Счётчики редких задач.
    //
    // Не всё в кадре нужно считать каждый кадр. Выбор музыкального
    // трека, индикатор загрузки и отметка «дошёл до места» меняются
    // единицы раз в минуту, а стоят обхода пулов ECS, десятков чтений
    // вокселей и восьми десятков поисков чанка под общим замком —
    // шестьдесят раз в секунду.
    //
    // Полями, а не статическими переменными внутри функции: те
    // переживали смену мира, и первый кадр нового отсчитывал время от
    // чужого значения.
    // ------------------------------------------------------------
    f32  ambienceTimer_    = 1e9f;   ///< музыкальный контекст, 4 раза в секунду
    f32  loadProgressTimer_= 1e9f;   ///< индикатор загрузки, 4 раза в секунду
    f32  locCheckTimer_    = 0.f;    ///< цели квестов «дойти до места»
    i32  cachedHostiles_   = 0;
    bool cachedUnderground_= false;

    /// Последний известный размер окна. В нём приходят касания и в
    /// нём же интерфейс считает свои прямоугольники.
    i32 winW_ = 0, winH_ = 0;

    /// Окно готово и приложение на переднем плане — можно рисовать.
    /// Два состояния держим раздельно: Android присылает фокус раньше,
    /// чем создаёт поверхность, и одного события мало, чтобы понять,
    /// можно ли рисовать.
    ///
    /// Начальное значение true: событие GAINED_FOCUS до инициализации
    /// окна пропадало впустую, второго не приходило, и цикл кадров не
    /// запускался вовсе — звук играл, экран оставался пустым.
    bool hasFocus = true;

    void updateRunning() {
        const bool want = initialized && hasFocus;
        if (want != running) {
            LOGI("цикл кадров: %s (окно=%d, фокус=%d)",
                 want ? "запущен" : "остановлен", (int)initialized, (int)hasFocus);
        }
        running = want;
    }
    bool wantQuit    = false;

    f32 unloadTimer = 0.f;
    f32 fps         = 0.f;
    u32 frameCount  = 0;
    std::chrono::steady_clock::time_point fpsTime;

    // ============================================================
    void onWindowInit(android_app* app) {
        ANativeWindow* w = app->window;
        if (!w) return;

        LOGI("=== VoxelRPG: инициализация окна (Phase 15) ===");

        activity = app->activity;
        internalDataPath = app->activity->internalDataPath ?
                           app->activity->internalDataPath : "/tmp";
        externalDataPath = app->activity->externalDataPath ?
                           app->activity->externalDataPath : "";
        settingsPath = internalDataPath + "/settings.cfg";

        crash::step("чтение настроек");
        cfg::settings().load(settingsPath);
        cfg::L().setLanguage(cfg::settingsConst().language);

        crash::step("Vulkan: создание контекста");
        // Режим показа надо выбрать ДО создания цепочки: это её
        // свойство. Иначе первая цепочка родится с ограничением по
        // экрану, а снялось бы оно только после первой смены тумблера.
        vk.setVsync(!cfg::settingsConst().unlimitedFps);
        if (!vk.init(w)) { LOGE("Vulkan init failed"); return; }

        const i32 ww = ANativeWindow_getWidth(w);
        const i32 wh = ANativeWindow_getHeight(w);
        winW_ = ww; winH_ = wh;

        crash::step("менеджер сохранений");
        saveMgr.init(internalDataPath.c_str());

        touch.setViewport(ww, wh);

        // Кнопки ещё не созданы — настройки ввода применятся в
        // setupButtons(), вместе с их раскладкой.

        crash::step("менеджер чанков");

        // Продолжаем прошлый мир, а если продолжать нечего —
        // создаём случайный.
        //
        // Зерно берётся из метаданных слота ЗАРАНЕЕ: иначе мир
        // строился бы дважды — сперва случайный, потом настоящий, —
        // и первый бросался бы тут же.
        {
            const auto& st = cfg::settingsConst();
            u64 seed = 0;
            const bool continuing = save::continueWorld(
                saveMgr.slots(), st.lastProfile, st.lastSlot, seed);
            if (!continuing) seed = world::randomWorldSeed();
            makeWorld(seed);
            continueSlot = continuing ? (i32)st.lastProfile : -1;
            LOGI(continuing ? "запуск: продолжаем прошлый мир"
                            : "запуск: прошлого мира нет, создан случайный");
        }

        crash::step("система рендера");
        render = std::make_unique<render::RenderSystem>();
        if (!render->init(vk, app->activity->assetManager)) {
            LOGE("RenderSystem init failed");
            return;
        }
        applySurfaceGeometry();
        render->camera().setFog(140.f, 380.f);
        render->camera().setSunDir(dayCycle.sunDirection());
        render->camera().setSky(dayCycle.skyColor(), dayCycle.skyLight(),
                                dayCycle.timeOfDay());

        crash::step("интерфейс");
        ui = std::make_unique<ui::UiSystem>();
        if (!ui->init(vk, app->activity->assetManager)) {
            LOGE("UI init failed");
            return;
        }
        // Плотность экрана. Без неё размеры считались от числа
        // пикселей, и цель касания выходила 20..36 dp при норме 48:
        // кнопка меню HUD была 3.2 мм при подушечке пальца 8..10 мм.
        ui->setDensityDpi(app->config ? AConfiguration_getDensity(app->config)
                                      : 0);
        ui->setScreenSize(ww, wh);
        ui->attachTouch(&touch);
        ui->setSurfaceRotation(vk.surfaceRotationDegrees());
        ui->showFps = cfg::settingsConst().showFps;

        ui->onQuit = [this]() { wantQuit = true; };
        ui->onSave = [this]() { doSave(lastSaveProfile, lastSaveSlot); };
        ui->onSaveRequested = [this](u32 p, u32 s) { doSave(p, s); };
        ui->onLoadRequested = [this](u32 p, u32 s) { doLoad(p, s); };
        ui->onDeleteRequested = [this](u32 p, u32 s) {
            auto slot = saveMgr.slots().slot(p, s);
            slot.removeFiles();

            // Удалённый мир перестаёт быть тем, который продолжают.
            // Без этого запуск честно попробовал бы его открыть,
            // не нашёл бы файла и создал случайный — но в настройках
            // так и осталась бы ссылка на пустоту.
            auto& st = cfg::settings();
            if (st.lastProfile == (i32)p && st.lastSlot == (i32)s) {
                st.lastProfile = -1;
                st.lastSlot    = -1;
                st.save(settingsPath);
            }

            ui->refreshSlotMeta(saveMgr.slots());
            ui->notify(cfg::T(cfg::StrKey::Notif_Deleted),
                       ui::theme::NotifyPriority::High);
        };

        ui->onCreateWorld = [this](const char* name, const char* seedText) {
            // Пустое поле зерна — не ошибка, а «мне всё равно»:
            // именно так мир и создают в первый раз.
            const u64 seed = (seedText && seedText[0] != '\0')
                           ? world::seedFromText(seedText)
                           : world::randomWorldSeed();
            makeWorld(seed);

            if (name && name[0] != '\0')
                std::snprintf(worldName, sizeof(worldName), "%s", name);
            else
                world::defaultWorldName(worldName, sizeof(worldName), seed);

            if (ui) {
                ui->currentWorldSeed = seed;
                ui->screen = ui::Screen::Hud;
                ui->returnTo = ui::Screen::Hud;
                ui->notify(cfg::T(cfg::StrKey::Notif_WorldCreated),
                           ui::theme::NotifyPriority::High);
            }
        };

        ui->onExportRequested = [this](u32 p, u32 s) {
            const std::string dir = save::exportDir(internalDataPath.c_str(),
                                                    externalDataPath.c_str());
            std::string outPath;
            const auto st = save::exportSlot(saveMgr.slots().slot(p, s),
                                             dir, outPath);
            if (!ui) return;
            if (st == save::TransferStatus::Ok) {
                // Путь в уведомлении не для красоты: без него игрок не
                // знает, где искать файл, и выгрузка бесполезна.
                ui->notify(std::string(cfg::T(cfg::StrKey::Notif_Exported)) +
                           ": " + outPath, ui::theme::NotifyPriority::High);
            } else {
                ui->notify(std::string(cfg::T(cfg::StrKey::Notif_ExportFailed)) +
                           ": " + save::transferStatusString(st),
                           ui::theme::NotifyPriority::High);
            }
        };

        ui->onImportRequested = [this](u32 p, u32 s) {
            if (!ui) return;

            // Системный выбор файла. Раньше его было нечем позвать —
            // диалог открывается через Activity, а Java в проекте не
            // было вовсе, — и файлы перебирались по кругу: игрок
            // клал мир в единственную известную игре папку и жал
            // кнопку, пока не попадётся нужный.
            importInto = { p, s };
            if (sys::openWorldPicker(activity)) return;

            // Диалога нет (запуск поверх голой NativeActivity) —
            // берём первый файл из общего каталога. Не изящно, зато
            // игра не остаётся вовсе без загрузки миров.
            importFromFolder(p, s);
        };

        ui->onCloseDialogue = [this]() {
            if (!player) return;
            npc::endDialogue(registry, (u32)player->entity());
        };

        ui->onSettingsChanged = [this]() {
            auto& s = cfg::settings();
            s.clamp();

            applyInputSettings();

            if (ui) ui->showFps = s.showFps;
            if (world) world->setViewDistance(s.viewDistance);

            // Режим показа — свойство цепочки, поэтому смена тумблера
            // пересоздаёт её. Context сам решит, когда: он поднимет
            // признак, beginFrame вернёт false, и главный цикл позовёт
            // onResize тем же путём, что и при повороте экрана.
            vk.setVsync(!s.unlimitedFps);

            cfg::L().setLanguage(s.language);

            // Audio volumes
            audio::engine().setMasterVolume(s.masterVolume);
            audio::engine().setMusicVolume(s.musicVolume);
            audio::engine().setSfxVolume(s.sfxVolume);

            s.save(settingsPath);
        };

        ui->onUseItem = [this](u32 slotIndex) {
            if (!player) return;
            auto res = player->useItem(*world, slotIndex);
            if (res == items::UseResult::Equipped) {
                audio::events().uiClick();
                ui->setStatus("Equipped");
            } else if (res == items::UseResult::NoEffect) {
                ui->notify("No effect", ui::theme::NotifyPriority::Low);
            } else if (res == items::UseResult::Consumed) {
                ui->notify("Consumed", ui::theme::NotifyPriority::Low);
            }
        };

        ui->onEquipHotbar = [this](u32 slotIndex) {
            if (!player) return;
            u8 i = (u8)(slotIndex - items::INV_HOTBAR_OFFSET);
            player->setActiveHotbar(i);
            audio::events().uiClick();
        };

        // Кнопка «в пояс» звала onMoveItem, а его никто не назначал:
        // std::function пустой, вызова нет, выделение снималось — и
        // предмет оставался на месте. Единственный коллбэк интерфейса
        // из двенадцати, оставшийся без обработчика.
        ui->onMoveItem = [this](u32 slotIndex, i32 dst) {
            if (!player || dst < 0) return;
            auto* inv = player->inventory();
            if (!inv) return;
            if (slotIndex >= items::INV_TOTAL_SLOTS ||
                (u32)dst >= items::INV_TOTAL_SLOTS) return;
            inv->swapSlots(slotIndex, (u32)dst);
            audio::events().uiClick();
        };

        ui->onDropItem = [this](u32 slotIndex) {
            if (!player) return;
            player->dropItem(slotIndex);
            audio::events().dropItem();
        };

        ui->onCraft = [this](u32 recipeId) {
            if (!player || !ui) return;
            auto* inv = player->inventory();
            auto* tree = player->skillTree();
            auto* prog = player->progression();
            if (!inv) return;

            crafting::CraftContext ctx{};
            ctx.inventory = inv;
            ctx.skillTree = tree;
            ctx.playerLevel = prog ? prog->level : 1;
            ctx.nearbyStation = ui->nearbyStation;
            ctx.craftTierBonus = player->derived().craftTierBonus;

            const auto& r = crafting::recipes().get((u16)recipeId);
            auto st = crafting::craft(ctx, r);
            if (st == crafting::CraftStatus::Ok) {
                audio::events().craft();
                ui->notify(cfg::T(cfg::StrKey::Notif_Crafted),
                               ui::theme::NotifyPriority::High);
            } else {
                audio::events().uiError();
                ui->setStatus(crafting::statusString(st));
            }
        };

        ui->onEnchant = [this](u32 recipeId) {
            if (!player || !ui) return;
            auto st = world::applyEnchantment(registry,
                                              player->entity(),
                                              (u16)recipeId);
            if (st == world::EnchantResult::Ok) {
                audio::events().enchant();
            }
            ui->setStatus(world::enchantResultString(st));
        };

        ui->onTradeBuy = [this](u16 itemId, u16 count) {
            if (!player || !ui) return;
            auto st = trade::buy(registry, player->entity(),
                                 (ecs::Entity)ui->tradeCtx.traderEntity,
                                 itemId, count);
            if (st == trade::TradeResult::Ok) {
                audio::events().pickupCoin();
            } else {
                audio::events().uiError();
            }
            ui->setStatus(trade::statusString(st));
        };

        ui->onTradeSell = [this](u16 itemId, u16 count) {
            if (!player || !ui) return;
            auto st = trade::sell(registry, player->entity(),
                                  (ecs::Entity)ui->tradeCtx.traderEntity,
                                  itemId, count);
            if (st == trade::TradeResult::Ok) {
                audio::events().pickupCoin();
            } else {
                audio::events().uiError();
            }
            ui->setStatus(trade::statusString(st));
        };

        render->setUiSystem(ui.get());

        touch.setUiRouter([this](i32 id, float px, float py, int phase) -> bool {
            if (!ui) return false;
            return ui->routeTouch(id, px, py, phase);
        });

        player = std::make_unique<player::Player>();
        player->init(registry, playerSpawn);

        if (auto* inv = player->inventory()) {
            items::ItemStack s;
            s.itemId = items::ITEM_POTION_HEALTH_SMALL; s.count = 3;
            inv->addStack(s);

            s.itemId = items::ITEM_BREAD; s.count = 5;
            inv->addStack(s);

            s.itemId = items::ITEM_STONE; s.count = 32;
            inv->addStack(s);

            s.itemId = items::ITEM_WOOD; s.count = 16;
            inv->addStack(s);

            s.itemId = items::ITEM_IRON_INGOT; s.count = 8;
            inv->addStack(s);

            // Факелы. Без них первая же ночь и первая же пещера —
            // это чёрный экран: свет в игре теперь настоящий, и взять
            // его в дорогу надо откуда-то. Дальше они крафтятся из
            // полена, прямо в поле.
            s.itemId = items::ITEM_TORCH; s.count = 16;
            inv->addStack(s);
        }

        spawner        = std::make_unique<mobs::Spawner>();
        trapSpawner    = std::make_unique<hazards::TrapSpawner>();
        npcSpawner     = std::make_unique<npc::NpcSpawner>();
        stationSpawner = std::make_unique<crafting::StationSpawner>();
        altarSpawner   = std::make_unique<world::EnchantAltarSpawner>();

        // ---- Phase 14: audio init ----
        if (audio::engine().init(48000)) {
            auto& s = cfg::settingsConst();
            audio::engine().setMasterVolume(s.masterVolume);
            audio::engine().setMusicVolume(s.musicVolume);
            audio::engine().setSfxVolume(s.sfxVolume);
            audio::events().setEngine(&audio::engine());
            musicDirector.init(audio::engine());
        } else {
            LOGW("Audio init failed — играем без звука");
        }

        setupButtons();
        ui->refreshSlotMeta(saveMgr.slots());

        // Прошлый мир дочитывается ПОСЛЕ того, как готово всё
        // остальное: doLoad трогает игрока, спавнеры и интерфейс, и
        // до этой точки их ещё нет.
        if (continueSlot >= 0) {
            const auto& st = cfg::settingsConst();
            doLoad((u32)st.lastProfile, (u32)st.lastSlot);
        }

        fpsTime = std::chrono::steady_clock::now();

        crash::step("инициализация завершена");
        initialized = true;
        updateRunning();
        running     = true;
        LOGI("=== VoxelRPG готов (Phase 15). Spawn y=%.1f ===", playerSpawn.y);
        // Метки времени GPU внутри прохода рендера стоят двух
        // миллисекунд на плиточном GPU и всё равно не делят кадр —
        // см. config::Settings::gpuPassTiming. По умолчанию их нет.
        vk.setPassTiming(cfg::settingsConst().gpuPassTiming);
        if (cfg::settingsConst().renderPassSweep && render) {
            render->passSweep().start();
            // Полная квалификация не для красоты: `render` здесь ещё
            // и поле Engine, и без ведущего «::» читателю приходится
            // вспоминать правило поиска имени перед «::».
            LOGI("развёртка проходов: включена, %u комбинаций по %.2f с, "
                 "%u кругов (~%.0f с), выход по завершении: %s",
                 ::render::PassSweep::STEP_COUNT,
                 (double)::render::PassSweep::SLICE_SEC,
                 ::render::PassSweep::ROUNDS,
                 (double)(::render::PassSweep::STEP_COUNT *
                          ::render::PassSweep::SLICE_SEC *
                          (f32)::render::PassSweep::ROUNDS),
                 cfg::settingsConst().exitAfterSweep ? "да" : "нет");
        }
        LOGI("зерно мира: %llu, дальность %d чанков",
             (unsigned long long)worldSeed, world->viewDistance());
    }

    /// Отпускает мир, не трогая остальное.
    ///
    /// Разрушение ChunkManager дожидается фоновых задач, а дождаться
    /// их можно только пока планировщик крутится. Поэтому мир обязан
    /// уйти раньше jobs::gJobs.stop() — в том числе на путях, где
    /// полная разборка не случилась: инициализация могла оборваться
    /// уже после создания мира (например, на неудаче рендера), и
    /// тогда мир доживал до деструктора Engine, то есть до момента,
    /// когда ждать уже некого.
    void releaseWorld() { world.reset(); }

    void onWindowTerm() {
        // Отладочная сцена — не мир игрока: записать её в автосейв
        // значит затереть настоящее сохранение ровной землёй.
        if (initialized && player && world && !cfg::settingsConst().debugScene) {
            LOGI("Автосейв при выходе...");
            doSave(save::SaveManager::AUTOSAVE_PROFILE,
                   save::SaveManager::AUTOSAVE_SLOT);
        }

        musicDirector.shutdown();
        audio::engine().shutdown();

        cfg::settings().save(settingsPath);

        // Журнал — в буфер обмена.
        //
        // Файл остаётся на месте, как и был; это не замена, а
        // избавление от похода в Termux ради чтения. Делаем здесь, а
        // не позже: на этом шаге приложение ещё на переднем плане, а
        // часть прошивок молча отказывает фоновым в записи в буфер.
        //
        // Сюда приходят оба пути выхода: и закрытие окна системой, и
        // выход из меню игры (wantQuit тоже зовёт onWindowTerm).
        if (activity && crash::logPath()[0]) {
            if (!sys::copyFileToClipboard(activity, crash::logPath()))
                LOGW("Журнал в буфер обмена не попал — файл на месте: %s",
                     crash::logPath());
        }

        // Окна больше нет — рисовать нельзя, чем бы ни был фокус.
        initialized = false;
        updateRunning();

        // Последние кадры ещё могут выполняться на GPU, а дальше мы
        // уничтожаем их буферы, конвейеры и текстуры. Ждать здесь
        // обязаны мы: vk.shutdown() ждёт слишком поздно, когда всё
        // уже уничтожено. Без этого закрытие приложения — это
        // уничтожение объектов, которыми драйвер прямо сейчас
        // пользуется.
        vk.waitIdle();

        if (render) {
            render->setUiSystem(nullptr);
            render->shutdown();
            render.reset();
        }
        altarSpawner.reset();
        stationSpawner.reset();
        npcSpawner.reset();
        spawner.reset();
        trapSpawner.reset();
        // У UiSystem нет деструктора, освобождающего ресурсы Vulkan:
        // атлас шрифта, его память и сэмплер, пул дескрипторов,
        // конвейер, модули шейдеров и вершинные буферы кадров жили до
        // конца процесса. Свернуть и развернуть приложение несколько
        // раз — и всё это накапливалось в видеопамяти.
        if (ui) ui->destroy();
        ui.reset();
        player.reset();
        world.reset();

        vk.shutdown();
    }

    /// Построить мир с этим зерном и выбрать точку появления.
    ///
    /// Всё, что должно случиться при СМЕНЕ мира, собрано здесь:
    /// и первый запуск, и «создать мир», и загрузка сейва с чужим
    /// зерном идут одной дорогой. Разложить это по трём местам —
    /// значит однажды забыть в одном из них про спавнеры, и жители
    /// прежнего мира останутся стоять в новом.
    void makeWorld(u64 seed) {
        // Меши старого мира лежат в видеопамяти и к новому рельефу
        // отношения не имеют. Забыть их обязан рендер, и поимённо —
        // иначе по тем же координатам покажется прежняя земля.
        if (world && render)
            for (const auto& c : world->loadedCoords()) render->forgetChunk(c);

        // Мобы, жители, лут и снаряды принадлежали ТОМУ миру. Игрок
        // переходит в новый со всем, что у него в карманах.
        if (player && registry.alive(player->entity()))
            registry.destroyAllExcept(player->entity());

        worldSeed = seed;
        world = std::make_unique<world::ChunkManager>(
            seed, cfg::settingsConst().viewDistance);
        world->setBlockModifyCallback([this](i32 wx, i32 wy, i32 wz, u16 newId) {
            worldDelta.recordBlock(wx, wy, wz, newId);
        });

        // Всё, что помнит про прошлый мир: изменённые блоки, вскрытые
        // тайники, время суток и спавнеры со своим «здесь уже было».
        worldDelta.clearAll();
        treasures      = hazards::TreasureKeeper{};
        dayCycle       = world::DayCycle{};
        playtime.set(0);
        spawner        = std::make_unique<mobs::Spawner>();
        trapSpawner    = std::make_unique<hazards::TrapSpawner>();
        npcSpawner     = std::make_unique<npc::NpcSpawner>();
        stationSpawner = std::make_unique<crafting::StationSpawner>();
        altarSpawner   = std::make_unique<world::EnchantAltarSpawner>();

        // Где появиться — не «в точке (4,4)»: там с равным успехом
        // море, обрыв или чужая стена. Перебор области сто на сто
        // блоков вокруг начала координат отвечает на этот вопрос
        // один раз и за всех, кто спрашивает.
        // Воксели спрашиваются по-настоящему: перебор по рельефу не
        // знает про деревья, и игрок просыпался внутри кроны.
        world::SpawnVoxelProbe probe(world->generator(), seed);
        playerSpawn = world::spawnPositionOrFallback(
            world->generator(), seed, 0, 0,
            [&probe](i32 wx, i32 wy, i32 wz) {
                return probe.standable(wx, wy, wz);
            });
        if (player) player->controller.setPosition(playerSpawn);

        // Погода у нового мира своя, и догоняется она сразу: плавный
        // переход от погоды ПРОШЛОГО мира не имел бы смысла.
        weather.init(seed);
        weather.snap(world->generator(), playerSpawn, dayCycle.worldSeconds());
        precip.reset();

        // Имя по умолчанию — чтобы мир было чем назвать в списке.
        // Игрок переименует его на экране создания, если захочет.
        world::defaultWorldName(worldName, sizeof(worldName), seed);
        if (ui) ui->currentWorldSeed = seed;

        LOGI("мир: зерно %llu, появление (%.1f, %.1f, %.1f), "
             "построено чанков для проверки места: %u",
             (unsigned long long)seed, (double)playerSpawn.x,
             (double)playerSpawn.y, (double)playerSpawn.z,
             probe.chunksBuilt());
    }

    /// Запомнить, в каком мире играли: с него начнётся следующий
    /// запуск. Пишется в настройки сразу — приложение на Android
    /// закрывают, не выходя из него.
    void rememberWorld(u32 profile, u32 slot) {
        auto& st = cfg::settings();
        if (st.lastProfile == (i32)profile && st.lastSlot == (i32)slot) return;
        st.lastProfile = (i32)profile;
        st.lastSlot    = (i32)slot;
        st.save(settingsPath);
    }

    /// Запасная загрузка: первый файл из общего каталога.
    void importFromFolder(u32 p, u32 s) {
        const std::string dir = save::exportDir(internalDataPath.c_str(),
                                                externalDataPath.c_str());
        const auto files = save::listExports(dir);
        if (!ui) return;
        if (files.empty()) {
            ui->notify(std::string(cfg::T(cfg::StrKey::Notif_ImportFailed)) +
                       ": " + dir, ui::theme::NotifyPriority::High);
            return;
        }
        applyImport(files[importPick % files.size()], p, s);
        ++importPick;
    }

    /// Положить выбранный файл в слот и сказать об этом игроку.
    void applyImport(const std::string& file, u32 p, u32 s) {
        const auto st = save::importSlot(file, saveMgr.slots().slot(p, s));
        if (!ui) return;
        if (st == save::TransferStatus::Ok) {
            ui->refreshSlotMeta(saveMgr.slots());
            ui->notify(cfg::T(cfg::StrKey::Notif_Imported),
                       ui::theme::NotifyPriority::High);
        } else {
            ui->notify(std::string(cfg::T(cfg::StrKey::Notif_ImportFailed)) +
                       ": " + save::transferStatusString(st),
                       ui::theme::NotifyPriority::High);
        }
    }

    void doSave(u32 profile, u32 slot) {
        if (!player || !world || !npcSpawner) return;
        auto sl = saveMgr.slots().slot(profile, slot);

        auto st = saveMgr.save(sl, *world, registry,
                               player->entity(), worldDelta,
                               worldSeed, playtime.seconds(), dayCycle,
                               *npcSpawner, treasures, worldName);

        if (st == save::SaveStatus::Ok) {
            lastSaveProfile = profile;
            lastSaveSlot    = slot;
            rememberWorld(profile, slot);
            if (ui) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s P%u S%u",
                              cfg::T(cfg::StrKey::Notif_Saved),
                              profile + 1, slot + 1);
                ui->setStatus(buf);
                ui->refreshSlotMeta(saveMgr.slots());
            }
        } else {
            if (ui) ui->notify(save::statusString(st),
                               ui::theme::NotifyPriority::High);
        }
    }

    void doLoad(u32 profile, u32 slot) {
        if (!player || !world || !npcSpawner) return;
        auto sl = saveMgr.slots().slot(profile, slot);

        // Сейв хранит зерно СВОЕГО мира, и загрузка в чужой мир
        // отбивается по несовпадению зерна. Пока зерно было
        // константой, этого не случалось никогда; со случайным зерном
        // при запуске не грузился бы ни один сейв. Поэтому мир
        // сначала перестраивается под сейв — и только потом чтение.
        save::SlotMeta meta;
        if (saveMgr.peekMeta(sl, meta) == save::SaveStatus::Ok &&
            meta.seed != worldSeed) {
            makeWorld(meta.seed);
        }

        u64 loadedSeed = worldSeed;
        u32 loadedPlaytime = 0;

        auto st = saveMgr.load(sl, *world, registry, player->entity(),
                               worldDelta, &loadedSeed, &loadedPlaytime,
                               &dayCycle, *npcSpawner, treasures);

        if (st == save::SaveStatus::Ok) {
            worldSeed = loadedSeed;
            playtime.set(loadedPlaytime);
            // Имя мира живёт в метаданных слота: мир, открытый из
            // списка, обязан и называться так же, как в списке.
            if (meta.worldName[0] != '\0')
                std::snprintf(worldName, sizeof(worldName), "%s", meta.worldName);
            if (ui) ui->currentWorldSeed = worldSeed;
            lastSaveProfile = profile;
            lastSaveSlot    = slot;
            rememberWorld(profile, slot);

            if (auto* tf = registry.get<ecs::Transform>(player->entity())) {
                player->controller.setPosition(tf->position);
            }
            // Время в загруженном мире другое, а погода считается от
            // него: без этого первые полминуты в мире стояла бы погода
            // того момента, когда сейв открывали.
            weather.snap(world->generator(),
                         player->controller.state().position,
                         dayCycle.worldSeconds());

            // Загруженная репутация — не событие: объявлять смену тира
            // за прошлую жизнь незачем.
            player->resyncReputationBaseline();
            // Здоровье из сейва — тоже не событие: трясти камеру за
            // урон прошлой жизни незачем.
            player->resyncImpactBaseline();
            if (ui) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s P%u S%u",
                              cfg::T(cfg::StrKey::Notif_Loaded),
                              profile + 1, slot + 1);
                ui->setStatus(buf);
                ui->setDialogueActive(false);
                ui->screen = ui::Screen::Hud;
            }
        } else {
            if (ui) ui->setStatus(save::statusString(st));
        }
    }

    // ============================================================
    // Экранные кнопки. Координаты — NDC с Y вверх: (-1,-1) — левый
    // нижний угол экрана. Порядок регистрации обязан совпадать с
    // config::ButtonSlot, иначе сохранённые сдвиги уедут не туда.
    // ============================================================
    /// Видны ли сейчас круглые кнопки. Меняется вместе с тем,
    /// открыто ли меню, и ровно тогда же меняется их доступность
    /// для касания.
    bool padButtonsVisible_ = true;

    void setupButtons() {
        // Положения и размеры — в ui/hud_layout.h, вместе со всей
        // остальной экранной геометрией. Здесь остаются только
        // действия: две копии чисел уже однажды разъехались, и
        // «CAM» оказалась поверх «ATT».
        if (!ui) return;
        const ui::HudLayout& L = ui->layout();
        auto add = [&](u32 slot, std::function<void(u32)> onPress) {
            const u32 id = touch.addButton(input::buttonCenterNdc(L, slot),
                                           input::buttonRadiusPx(L, slot),
                                           std::move(onPress), nullptr);
            // Шрифт HUD знает только латиницу до 95-го кода, поэтому
            // подписи короткие и заглавными.
            touch.setButtonLabel(id, input::buttonLabel(slot));
            return id;
        };

        btnAttack_   = add(cfg::Btn_Attack,   [this](u32) { evAttack_   = true; });
        btnFinisher_ = add(cfg::Btn_Finisher, [this](u32) { evFinish_   = true; });
        // Защита читается УДЕРЖАНИЕМ (isButtonHeld), но обработчик
        // нажатия всё равно обязателен: кнопка без действия не
        // доходит до геймпада и не видна проверке раскладки. Здесь
        // он поднимает защиту в тот же кадр, что и касание, — иначе
        // окно парирования начиналось бы на кадр позже.
        btnBlock_    = add(cfg::Btn_Block,    [this](u32) { evBlock_ = true; });
        // Обработчик обязателен: без него нажатие никуда не идёт.
        // Кнопка прыжка стояла с nullptr, и evJump_ не выставлялся
        // НИКОГДА — ни с экрана, ни с геймпада. Прыжок по кнопке не
        // происходил вовсе; работало только удержание в воде, где
        // читается jumpHeld, а не jumpPressed.
        btnJump_     = add(cfg::Btn_Jump,     [this](u32) { evJump_ = true; });
        btnBreak_    = add(cfg::Btn_Break,    [this](u32) { evBreak_    = true; });
        btnPlace_    = add(cfg::Btn_Place,    [this](u32) { evPlace_    = true; });
        btnInteract_ = add(cfg::Btn_Interact, [this](u32) { evInteract_ = true; });
        btnUseItem_  = add(cfg::Btn_UseItem,  [this](u32) { evUseItem_  = true; });
        btnDash_     = add(cfg::Btn_Dash,     [this](u32) { evDash_ = true; });
        btnCamera_   = add(cfg::Btn_Camera,   [this](u32) {
                if (!player) return;
                player->cameraMode =
                    (player->cameraMode == player::CameraMode::FirstPerson)
                        ? player::CameraMode::ThirdPerson
                        : player::CameraMode::FirstPerson;
            });

        buttonIds_[cfg::Btn_Attack]   = btnAttack_;
        buttonIds_[cfg::Btn_Finisher] = btnFinisher_;
        buttonIds_[cfg::Btn_Block]    = btnBlock_;
        buttonIds_[cfg::Btn_Jump]     = btnJump_;
        buttonIds_[cfg::Btn_Break]    = btnBreak_;
        buttonIds_[cfg::Btn_Place]    = btnPlace_;
        buttonIds_[cfg::Btn_Interact] = btnInteract_;
        buttonIds_[cfg::Btn_UseItem]  = btnUseItem_;
        buttonIds_[cfg::Btn_Camera]   = btnCamera_;
        buttonIds_[cfg::Btn_Dash]     = btnDash_;

        // Перетаскивание кнопки сразу сохраняется в настройки.
        touch.setLayoutCallback([this](u32 buttonId, glm::vec2 off) {
            auto& s = cfg::settings();
            for (u32 i = 0; i < cfg::Settings::BUTTON_SLOTS; ++i) {
                if (buttonIds_[i] != buttonId) continue;
                s.buttonOffsetX[i] = off.x;
                s.buttonOffsetY[i] = off.y;
                s.save(settingsPath);
                break;
            }
        });

        applyInputSettings();
    }

    /// Переносит настройки управления в TouchInput. Вызывается при
    /// старте и после каждого изменения в меню.
    void applyInputSettings() {
        const auto& s = cfg::settingsConst();
        touch.setCameraSensitivity(s.cameraSensitivity);
        touch.invertX(s.invertX);
        touch.invertY(s.invertY);
        touch.setJoystickLeftHanded(s.joystickLeftHanded);
        touch.setJoystickRadius(s.joystickRadius);
        touch.setJoystickDeadzone(s.joystickDeadzone);
        touch.setJoystickOpacity(s.joystickOpacity);
        touch.setButtonScale(s.buttonScale);

        // ТЗ 5.2: свободное перемещение кнопок по экрану.
        for (u32 i = 0; i < cfg::Settings::BUTTON_SLOTS; ++i) {
            if (!buttonIds_[i]) continue;
            touch.setButtonOffset(buttonIds_[i],
                                  { s.buttonOffsetX[i], s.buttonOffsetY[i] });
        }
    }

    /// Сколько враждебных мобов рядом преследуют игрока или атакуют.
    /// Музыкальный директор поднимает напряжение по этому числу.
    i32 hostilesNearby() const {
        if (!player) return 0;
        const glm::vec3 ppos = player->controller.state().position;
        auto& reg = const_cast<ecs::Registry&>(registry);
        auto& pool = reg.pool<ecs::AIAgent>();
        i32 count = 0;
        for (usize i = 0; i < pool.size(); ++i) {
            const auto& agent = pool.at((u32)i);
            if (agent.state != ecs::AIAgent::Chase &&
                agent.state != ecs::AIAgent::Attack) continue;
            const ecs::Entity e = pool.entityAt((u32)i);
            if (!reg.has<mobs::MobTag>(e)) continue;
            auto* tf = reg.get<ecs::Transform>(e);
            if (!tf) continue;
            const glm::vec3 d = tf->position - ppos;
            if (glm::dot(d, d) < 32.f * 32.f) ++count;
        }
        return count;
    }

    /// Под землёй ли игрок: есть ли над головой непрозрачное
    /// перекрытие. Определяет выбор музыкального трека.
    bool isUnderground(const glm::vec3& pos) const {
        if (!world) return false;
        const i32 x = (i32)std::floor(pos.x);
        const i32 z = (i32)std::floor(pos.z);
        const i32 y0 = (i32)std::floor(pos.y) + 2;
        for (i32 y = y0; y < world::CHUNK_SIZE_Y; ++y) {
            const u16 b = world->getVoxel(x, y, z);
            if (b != world::AIR && !world::blocks().isTransparent(b)) return true;
        }
        return false;
    }

    /// Переносит геометрию поверхности в камеру и интерфейс.
    ///
    /// Три разные вещи, которые легко перепутать:
    ///  * размер ОКНА (ANativeWindow) — в нём приходят касания и в нём
    ///    же интерфейс считает свои прямоугольники;
    ///  * размер БУФЕРА кадра (swapchain) — в нём живёт gl_FragCoord,
    ///    и именно его ждёт шейдер неба;
    ///  * поворот при выводе — композитор довернёт кадр, и повернуть
    ///    содержимое обязаны мы сами.
    /// Раньше в шейдер уезжал размер окна, а поворот не учитывался
    /// нигде: мир лежал на боку, а небо строилось по чужой сетке.
    void applySurfaceGeometry() {
        if (!render) return;
        const VkExtent2D fb = vk.extent();
        if (fb.width == 0 || fb.height == 0) return;

        const f32 fw = (f32)fb.width, fh = (f32)fb.height;
        // Соотношение берём прямо у буфера кадра. Лог с устройства
        // показал, что буфер приходит уже в ориентации окна
        // (2306x1080 при окне 2306x1080), то есть менять местами
        // стороны не надо: от этого получалось 0.468 вместо 2.135, и
        // мир сплющивался поперёк. Когда композитор доворачивает сам —
        // а мы его об этом и просим, — переставлять нечего тем более.
        const f32 logicalAspect = fw / fh;

        // Отдельной строкой, потому что различить эти три числа на
        // глаз невозможно, а от их соотношения зависит, растянут ли
        // интерфейс. Если окно и буфер повёрнуты по-разному, окно
        // живёт в логических координатах; если одинаково — в
        // координатах буфера, и раскладка интерфейса окажется в
        // портретной коробке, растянутой на альбомный экран.
        LOGI("Поверхность: окно %dx%d, буфер %ux%u, наш доворот %u, соотношение %.3f",
             winW_, winH_, fb.width, fb.height, vk.surfaceRotationDegrees(),
             (double)logicalAspect);

        auto& cam = render->camera();
        cam.setAspect(logicalAspect);
        cam.setViewport(fb.width, fb.height);
        cam.setSurfaceRotation(vk.surfaceRotationDegrees());
        if (ui) ui->setSurfaceRotation(vk.surfaceRotationDegrees());
    }

    /// Кадр отладочной сцены: камера, подгрузка чанков и больше ничего.
    void updateDebugScene(f32 timeSec) {
        (void)timeSec;
        const glm::vec3 eye{ world::SCENE_EYE_X, world::SCENE_EYE_Y,
                             world::SCENE_EYE_Z };

        auto& cam = render->camera();
        cam.setDebugCamera(eye, world::SCENE_YAW, world::SCENE_PITCH);
        cam.setSunDir({ world::SCENE_SUN_X, world::SCENE_SUN_Y, world::SCENE_SUN_Z });
        cam.setSky({ world::SCENE_SKY_R, world::SCENE_SKY_G, world::SCENE_SKY_B },
                   world::SCENE_SKY_LIGHT, world::SCENE_TIME_OF_DAY);

        // Чанки грузятся вокруг камеры, а не вокруг игрока: игрок в
        // этом режиме не двигается и не существует для сцены.
        world->update(eye);

        // Игрока держим ровно под камерой и неподвижным: его позицию
        // читают подсистемы, которые здесь не работают, но пусть она
        // будет осмысленной, а не той, где его оставил генератор.
        player->controller.setPosition(eye);

        // Касания принимаются, но никуда не идут: буферы всё равно
        // надо закрывать покадрово, иначе состояние пальцев копится.
        evJump_ = evBreak_ = evPlace_ = evAttack_ = evFinish_ =
            evInteract_ = evUseItem_ = evDash_ = evBlock_ = false;
        touch.endFrame();

        debugSceneLog();
    }

    /// Доказательство изоляции. Печатается раз в секунду: если хоть
    /// одно число поедет между кадрами, стенд негоден и это видно
    /// сразу, а не после сравнения картинок.
    void debugSceneLog() {
        debugSceneTimer_ += world::SCENE_FIXED_DT;
        if (debugSceneTimer_ < 1.f && debugSceneFrames_ != 0) {
            ++debugSceneFrames_;
            return;
        }
        debugSceneTimer_ = 0.f;
        ++debugSceneFrames_;

        const glm::vec3 p = render->camera().position();
        LOGI("debug_scene=true кадр %llu | камера %.3f %.3f %.3f, "
             "yaw %.4f pitch %.4f | время мира %.3f, шаг кадра %.4f | "
             "мобы %zu, NPC %zu, предметы %zu, снаряды %zu | "
             "чанков загружено %zu, нарисовано %u",
             (unsigned long long)debugSceneFrames_,
             (double)p.x, (double)p.y, (double)p.z,
             (double)render->camera().yaw(), (double)render->camera().pitch(),
             (double)world::SCENE_TIME_SEC, (double)world::SCENE_FIXED_DT,
             registry.pool<mobs::MobAI>().size(),
             registry.pool<npc::NpcAI>().size(),
             registry.pool<items::ItemPickup>().size(),
             registry.pool<combat::Projectile>().size(),
             world->loadedChunks(),
             render->drawnChunks());
    }

    void update(f32 dt, f32 timeSec) {
        if (!player || !world || !render) return;

        // Отладочная сцена — отдельный, очень короткий путь.
        //
        // Стенд считается пригодным, только если в нём НИЧЕГО не
        // шевелится само: иначе два кадра нельзя сравнить, а
        // расхождение нельзя приписать рендеру. Поэтому здесь не
        // выполняется ни одна игровая система — ни ввод, ни физика
        // игрока, ни сутки, ни появление мобов и NPC, ни их ИИ, ни
        // предметы, ни снаряды, ни бой, ни задания, ни автосейв.
        // Остаётся ровно то, без чего сцены не будет: подгрузка
        // чанков вокруг неподвижной камеры.
        if (cfg::settingsConst().debugScene) {
            updateDebugScene(timeSec);
            return;
        }

        playtime.tick(dt);
        dayCycle.tick(dt);

        // Выбранный в системном диалоге файл приходит из потока Java
        // и ждёт, пока его заберут. Забираем здесь: игровой цикл —
        // единственное место, где можно трогать сейвы.
        {
            const std::string picked = sys::takePickedFile();
            if (!picked.empty())
                applyImport(picked, importInto.profile, importInto.slot);
        }

        if (world && player) {
            weather.update(world->generator(),
                           player->controller.state().position,
                           dayCycle.worldSeconds(),
                           dayCycle.sunElevation(), dt);

            // Осадки идут вокруг ГЛАЗ, а не вокруг ног: от третьего
            // лица камера стоит на пару блоков выше и позади, и
            // столб дождя вокруг ступней в кадр попадал бы краем.
            const glm::vec3 eye = render
                ? render->camera().position()
                : player->controller.state().position;
            precip.update(*world, eye, weather.precip(), weather.snowMix(),
                          weather.wind(), dt);

            // Дождь слышно. Снег — нет: он и в жизни беззвучен, и
            // шипение под снегопадом читалось бы как дождь.
            audio::events().setRain(weather.precip() *
                                    (1.f - weather.snowMix()));
        }

        // ТЗ 4.4: торговец обновляет ассортимент раз в игровой день.
        if (dayCycle.dayJustChanged()) {
            auto& shops = registry.pool<trade::TradeInventory>();
            for (usize i = 0; i < shops.size(); ++i) shops.at((u32)i).restock();
            LOGI("Новый игровой день: %u, ассортимент торговцев обновлён",
                 dayCycle.day());
        }

        const auto& s = cfg::settingsConst();
        if (s.autosaveEnabled) {
            autosaveTimer += dt;
            if (autosaveTimer >= s.autosaveInterval) {
                autosaveTimer = 0.f;
                doSave(save::SaveManager::AUTOSAVE_PROFILE,
                       save::SaveManager::AUTOSAVE_SLOT);
            }
        }

        const bool uiBlockingInput = ui && ui->paused();

        // Кнопку, которую не видно, нельзя нажать.
        //
        // Рисуются круглые кнопки только поверх чистого HUD, а ловили
        // касание всегда: setButtonVisible не вызывал никто, и все
        // они оставались visible навсегда. Фон инвентаря не является
        // интерактивным прямоугольником, поэтому тап по пустому месту
        // проваливался сквозь меню на невидимую кнопку — включая DIG
        // и PUT, которые меняют мир.
        if (padButtonsVisible_ != !uiBlockingInput) {
            padButtonsVisible_ = !uiBlockingInput;
            for (u32 i = 0; i < cfg::Settings::BUTTON_SLOTS; ++i)
                if (buttonIds_[i])
                    touch.setButtonVisible(buttonIds_[i], padButtonsVisible_);
            // Зажатую кнопку и джойстик тоже снимаем: иначе палец,
            // опущенный до открытия меню, остаётся «нажатым» внутри
            // него и отпускается уже неизвестно где.
            if (!padButtonsVisible_) touch.cancelAll();
        }

        auto* dlg = player->activeDialogue();
        bool dialogueActive = dlg && dlg->active;
        if (ui) ui->setDialogueActive(dialogueActive);

        // Диалог мог попросить открыть экран — сам он этого не умеет,
        // об интерфейсе он не знает. Раньше выбор «покажи товар»
        // просто закрывал диалог, и торговля оставалась недостижимой.
        if (dlg && ui && dlg->pendingAction != npc::DialogueAction::None) {
            const auto act = dlg->pendingAction;
            dlg->pendingAction = npc::DialogueAction::None;
            if (act == npc::DialogueAction::OpenTrade) {
                ui->openTrade(dlg->npcEntity);
            } else if (act == npc::DialogueAction::OpenCraft) {
                // Кузнец работает как переносная мастерская: свой
                // станок рядом с ним искать не надо.
                ui->openCrafting(crafting::StationType::Anvil);
            }
        }

        // Phase 15: обновляем spatial hash раз в кадр (лениво).
        spatialHash.tick();

        // Ориентация сущностей.
        //
        // Один проход на кадр: направление движения берётся из
        // скорости, а модельный yaw догоняет его с ограниченной
        // угловой скоростью по кратчайшей дуге. Рендер это состояние
        // только ЧИТАЕТ — раньше он вычислял угол сам, каждый кадр
        // заново, и обнулял при остановке.
        {
            auto& facings = registry.pool<ecs::Facing>();
            for (usize i = 0; i < facings.size(); ++i) {
                ecs::Entity fe = facings.entityAt((u32)i);
                auto* fc = facings.get(fe);
                auto* fv = registry.get<ecs::Velocity>(fe);
                if (!fc || !fv) continue;
                orient::advanceFacing(*fc, fv->linear, dt);

                // Фаза шага — здесь же и из той же скорости. Раньше её
                // двигал ИИ строчками `walkPhase += dt * 9.f`, своими
                // на каждое состояние: скорость и длина шага не были
                // связаны ничем, и ноги скользили.
                if (auto* g = registry.get<ecs::Gait>(fe))
                    anim::advanceGait(*g, fv->linear, dt);

                // Состояние передвижения — оттуда же. Выбирается по
                // фактической скорости и признаку опоры, а не по
                // состоянию ИИ: преследовать можно и в падении.
                if (auto* lo = registry.get<ecs::Locomotion>(fe)) {
                    const f32 sp = std::sqrt(fv->linear.x * fv->linear.x +
                                             fv->linear.z * fv->linear.z);
                    f32 maxSp = 6.f;
                    if (auto* mt = registry.get<mobs::MobTag>(fe))
                        maxSp = std::max(0.5f,
                                mobs::mobRegistry().get(mt->id).chaseSpeed);
                    else if (auto* nt = registry.get<npc::NpcTag>(fe))
                        maxSp = std::max(0.5f,
                                npc::npcRegistry().get(nt->id).moveSpeed);
                    anim::advanceLocomotion(*lo, std::min(1.f, sp / maxSp),
                                            fv->linear.y, lo->grounded, dt);
                }
            }
        }

        if (!uiBlockingInput) {
            // Обзор. Знаки здесь были плюсовые, и получалось «тяну
            // мир за собой»: палец вправо — камера влево, палец вниз —
            // взгляд вверх. Привычно наоборот: куда ведёшь палец, туда
            // и смотришь. Проверяется прямо: рост yaw уводит объекты
            // вправо по экрану (значит, камера поворачивается влево),
            // рост pitch — вниз (камера задирается вверх).
            const glm::vec2 camDelta = touch.cameraDelta();
            cameraYawPitch.x -= camDelta.x * 3.0f;
            cameraYawPitch.y = glm::clamp(
                cameraYawPitch.y - camDelta.y * 3.0f, -1.5f, 1.5f);

            player::PlayerInput pin;
            pin.moveAxis      = touch.moveAxis();
            pin.jumpPressed   = evJump_;
            pin.jumpHeld      = touch.isButtonHeld(btnJump_);
            // Бег — двойное нажатие по джойстику, а не кнопка.
            pin.sprint        = touch.sprintActive();
            pin.attackPressed = evAttack_;
            pin.attackHeld    = touch.isButtonHeld(btnAttack_);
            pin.finisherInput = evFinish_;
            pin.interactPressed = evInteract_;
            pin.dashPressed   = evDash_;
            // Удержание ИЛИ нажатие этого кадра: касание и первый
            // опрос удержания приходятся на разные кадры, и без «или»
            // защита вставала бы на кадр позже касания — а окно
            // парирования всего в двести двадцать миллисекунд.
            pin.blockHeld     = touch.isButtonHeld(btnBlock_) || evBlock_;

            // Записываем скорость до апдейта (для звука шагов).
            glm::vec3 prevPos = player->controller.state().position;

            // Phase 15: обновляем spatial hash перед боем
            spatialHash.ensureFresh(registry);

            // Пока чанк под игроком не сгенерирован, мир о нём ничего
            // не знает: всё вокруг считается камнем, и столкновения
            // вытолкнут игрока вверх. Ждём — это доли секунды на старте.
            const glm::vec3 prePos = player->controller.state().position;
            const bool groundKnown = world->isReadyAt((i32)std::floor(prePos.x),
                                                      (i32)std::floor(prePos.z));
            if (!groundKnown) {
                if (!waitingForGround) {
                    waitingForGround = true;
                    LOGI("Жду генерацию чанка под игроком (%.1f %.1f)",
                         (double)prePos.x, (double)prePos.z);
                }
            } else {
                if (waitingForGround) {
                    waitingForGround = false;
                    LOGI("Чанк под игроком готов, физика включена");
                }

                player->updateWithHash(*world, &spatialHash, pin, dt,
                                       cameraYawPitch.x, cameraYawPitch.y);

                // Смерть, возвращение и запомненный колодец — игрок
                // должен узнать о каждом.
                if (ui) {
                    if (player->justDied)
                        ui->notify(cfg::T(cfg::StrKey::Notif_Died),
                                   ui::theme::NotifyPriority::High);
                    if (player->justRespawned)
                        ui->notify(cfg::T(cfg::StrKey::Notif_Respawned),
                                   ui::theme::NotifyPriority::High);
                    if (player->newRespawnPoint)
                        ui->notify(cfg::T(cfg::StrKey::Notif_WellBound),
                                   ui::theme::NotifyPriority::Normal);
                    // Логово: без слова об этом «вокруг одни волки»
                    // читается как поломка спавна.
                    // Удар о землю: без слова игрок не поймёт, за
                    // что у него убавилось здоровья.
                    if (player->lastFallDamage > 0.f)
                        ui->notify(cfg::T(cfg::StrKey::Notif_Fall),
                                   ui::theme::NotifyPriority::High);
                    // Деревня: чем она живёт, видно по домам, но
                    // сказать об этом словами дешевле и вернее.
                    if (player->justDrowned)
                        ui->notify(cfg::T(cfg::StrKey::Notif_Drowning),
                                   ui::theme::NotifyPriority::High);
                    // Про парирование рассказываем ровно один раз и
                    // ровно тогда, когда игрок уже что-то принял на
                    // защиту: до первого блока объяснять нечего, а
                    // после него у него в руках половина механики и
                    // не хватает второй.
                    if (player->pendingParryHint) {
                        player->pendingParryHint = false;
                        ui->notify(cfg::T(cfg::StrKey::Hint_Parry),
                                   ui::theme::NotifyPriority::High);
                    }
                    if (player->enteredVillage) {
                        cfg::StrKey k = cfg::StrKey::Notif_VillageFarmstead;
                        switch (player->villageStyle) {
                            case world::VillageStyle::Stonemason:
                                k = cfg::StrKey::Notif_VillageStonemason; break;
                            case world::VillageStyle::Garrison:
                                k = cfg::StrKey::Notif_VillageGarrison;   break;
                            case world::VillageStyle::Woodland:
                                k = cfg::StrKey::Notif_VillageWoodland;   break;
                            default: break;
                        }
                        ui->notify(cfg::T(k), ui::theme::NotifyPriority::Normal);
                    }
                    if (player->enteredLair) {
                        cfg::StrKey k = cfg::StrKey::Notif_LairWolves;
                        switch (player->lairKind) {
                            case world::LairKind::Skeletons:
                                k = cfg::StrKey::Notif_LairSkeletons; break;
                            case world::LairKind::Goblins:
                                k = cfg::StrKey::Notif_LairGoblins;   break;
                            case world::LairKind::Slimes:
                                k = cfg::StrKey::Notif_LairSlimes;    break;
                            default: break;
                        }
                        ui->notify(cfg::T(k), ui::theme::NotifyPriority::High);
                    }
                }

                // Спасение из-под мира. Провалиться теперь неоткуда — ниже
                // нулевой отметки мир отвечает камнем, — но сохранения,
                // сделанные до этой починки, хранят игрока далеко внизу, да
                // и любая будущая щель в столкновениях не должна означать
                // падение без возврата.
                const glm::vec3 p = player->controller.state().position;
                if (p.y < 0.f || p.y > (f32)world::CHUNK_SIZE_Y + 16.f) {
                    const i32 surf = world->generator().surfaceHeight(
                        (i32)std::floor(p.x), (i32)std::floor(p.z));
                    const glm::vec3 rescued{ p.x, (f32)surf + 1.5f, p.z };
                    LOGW("Игрок вне мира (y=%.1f) — возвращён на поверхность y=%.1f",
                         (double)p.y, (double)rescued.y);
                    player->controller.setPosition(rescued);
                }
            }

            // Phase 14: звук шагов
            if (player->controller.state().onGround) {
                f32 hs = glm::length(glm::vec2(
                    player->controller.state().velocity.x,
                    player->controller.state().velocity.z));
                if (hs > 0.6f) {
                    footstepTimer -= dt;
                    if (footstepTimer <= 0.f) {
                        footstepTimer = std::max(0.20f, 0.55f - hs * 0.05f);

                        glm::vec3 p = player->controller.state().position;
                        i32 bx = (i32)std::floor(p.x);
                        i32 by = (i32)std::floor(p.y - 0.1f);
                        i32 bz = (i32)std::floor(p.z);
                        u16 block = world->getVoxel(bx, by, bz);
                        audio::events().footstep(block, p);
                    }
                } else {
                    footstepTimer = 0.f;
                }
            } else {
                footstepTimer = 0.f;
            }
            (void)prevPos;

            if (evUseItem_) {
                if (auto* inv = player->inventory()) {
                    u32 slot = items::INV_HOTBAR_OFFSET + inv->activeHotbar;
                    player->useItem(*world, slot);
                }
            }

            if (evPlace_) {
                u16 blockId = player->selectedBlock();
                if (blockId != 0) {
                    glm::vec3 before = player->controller.state().position;
                    if (player->tryPlaceBlock(*world, blockId)) {
                        glm::vec3 d = player->aimDir();
                        glm::vec3 target = player->controller.state().position +
                                           glm::vec3(0.f, 1.3f, 0.f) + d * 2.f;
                        audio::events().blockPlace(blockId, target);
                    }
                    (void)before;
                }
            }

            if (evBreak_) {
                glm::vec3 d = player->aimDir();
                glm::vec3 target = player->controller.state().position +
                                   glm::vec3(0.f, 1.3f, 0.f) + d * 2.f;
                physics::RayHit hit = player->targetBlock(*world);
                if (player->tryBreakBlock(*world)) {
                    audio::events().blockBreak(hit.blockType, target);
                } else if (hit.hit) {
                    // Удар пришёлся, но блок остался: коренная порода.
                    // Раньше это была полная тишина — неотличимая от
                    // «палец не попал». hitBlock() как раз для такого
                    // и написан, и до сих пор его никто не звал.
                    audio::events().hitBlock(hit.blockType, target);
                }
            }

            world->update(player->controller.state().position);

            // Phase 15: обновление пикапов с raycast-коллизией.
            items::updatePickups(*world, registry, player->entity(),
                                 player->controller.state().position, dt);
            items::updateTrampolines(registry, dt);

            if (ui) {
                ui->nearbyStation = crafting::detectNearbyStation(
                    registry, player->controller.state().position, 3.0f);

                ui->nearbyAltar = 0;
                {
                    const glm::vec3 pos = player->controller.state().position;
                    auto& pool = registry.pool<world::EnchantAltar>();
                    f32 bestD2 = 9.f;
                    for (usize k = 0; k < pool.size(); ++k) {
                        ecs::Entity e = pool.entityAt((u32)k);
                        auto* tf = registry.get<ecs::Transform>(e);
                        if (!tf) continue;
                        glm::vec3 d = tf->position - pos;
                        d.y = 0.f;
                        f32 d2 = glm::dot(d, d);
                        if (d2 < bestD2) {
                            bestD2 = d2;
                            ui->nearbyAltar = (u32)e;
                        }
                    }
                }
            }

            unloadTimer += dt;
            if (unloadTimer >= 2.0f) {
                unloadTimer = 0.f;
                const glm::vec3 pp = player->controller.state().position;
                auto toUnload = world->collectUnloadCandidates(pp);

                // Сверх круга — ещё и потолок памяти. Круг считает
                // чанки, потолок считает мегабайты, и второе система
                // спрашивает строже: при дальности 12 круг держит под
                // 450 чанков, это 115 МБ одних вокселей, а на планшете
                // дальность больше и предела не было вовсе.
                //
                // Тем же путём, что и обычная выгрузка: меши этих
                // чанков лежат в видеопамяти, и забыть их обязан ещё
                // и рендер.
                auto overBudget = world->collectOverBudget(pp);
                if (!overBudget.empty()) {
                    LOGI("память мира: %zu МБ при потолке %zu МБ — "
                         "выгружаем %zu дальних чанков",
                         world->residentBytes() >> 20,
                         world->memoryBudget()  >> 20,
                         overBudget.size());
                    toUnload.insert(toUnload.end(),
                                    overBudget.begin(), overBudget.end());
                    std::sort(toUnload.begin(), toUnload.end(),
                              [](const world::ChunkCoord& a,
                                 const world::ChunkCoord& b) {
                                  return a.x != b.x ? a.x < b.x : a.z < b.z;
                              });
                    toUnload.erase(std::unique(toUnload.begin(), toUnload.end()),
                                   toUnload.end());
                }

                if (!toUnload.empty()) {
                    for (const auto& c : toUnload) render->forgetChunk(c);
                    world->removeChunks(toUnload);
                }
            }
        }

        // Камера
        auto& cam = render->camera();
        cam.setTargetPosition(player->controller.state().position);
        cam.setFirstPerson(player->cameraMode == player::CameraMode::FirstPerson);
        cam.setFirstPersonEye(player->firstPersonEye);
        cam.setThirdPersonDistance(player->thirdPersonDistance);
        cam.setThirdPersonHeight(player->thirdPersonHeight);
        cam.setHeadBob(player->bobPhase, player->bobAmount);
        // Отдача камеры: игрок за кадр уже узнал и о своём попадании,
        // и о своей боли — камере остаётся забрать накопленное.
        cam.addShake(player->takeCameraShake());
        cam.tickShake(dt);
        cam.setYawPitch(cameraYawPitch.x, cameraYawPitch.y);
        cam.setSunDir(dayCycle.sunDirection());
        cam.setSky(dayCycle.skyColor(), dayCycle.skyLight(), dayCycle.timeOfDay());
        cam.setWeather(weather.cloud(), weather.precip(), weather.rainbow(),
                       weather.snowMix(), weather.wind());

        // Минимальная детерминированная сцена: камера, солнце и небо
        // прибиты к числам из world/debug_scene.h — тем самым, по
        // которым tools/vkcheck --scene minimal рисует свой кадр.
        // Подвижная камера, ходящее солнце и качающаяся от времени
        // не дали бы сравнить два кадра никогда.
        if (config::settingsConst().debugScene) {
            cam.setFirstPerson(true);
            cam.setHeadBob(0.f, 0.f);
            cam.setFirstPersonEye(0.f);
            cam.setThirdPersonDistance(0.f);
            cam.setTargetPosition({ world::SCENE_EYE_X, world::SCENE_EYE_Y,
                                    world::SCENE_EYE_Z });
            cam.setYawPitch(world::SCENE_YAW, world::SCENE_PITCH);
            cam.setSunDir({ world::SCENE_SUN_X, world::SCENE_SUN_Y,
                            world::SCENE_SUN_Z });
            cam.setSky({ world::SCENE_SKY_R, world::SCENE_SKY_G,
                         world::SCENE_SKY_B },
                       world::SCENE_SKY_LIGHT, world::SCENE_TIME_OF_DAY);
        }
        cam.followTarget(*world, player->aimDir());

        // Мир
        if (spawner && player) {
            const glm::vec3 ppos = player->controller.state().position;
            spawner->update(*world, registry, ppos, dayCycle, worldSeed, dt);
            mobs::updateMobs(*world, registry, player->entity(), ppos, dt);
        }
        if (npcSpawner && player) {
            const glm::vec3 ppos = player->controller.state().position;
            npcSpawner->update(*world, registry, ppos, worldSeed);
            npc::updateNpcs(*world, registry, player->entity(), ppos, dt);
        }
        // Ловушки: сперва заводим и перезаряжаем, потом смотрим,
        // не наступил ли кто. В обратном порядке только что
        // заведённая ловушка срабатывала бы в тот же кадр, ещё до
        // того, как игрок её увидит.
        if (trapSpawner && player) {
            const glm::vec3 ppos = player->controller.state().position;
            trapSpawner->update(*world, registry, ppos, worldSeed, dt);
            const f32 hurt = hazards::tickTraps(registry, player->entity(),
                                                ppos, dt);
            if (hurt > 0.f && ui)
                ui->notify(cfg::T(cfg::StrKey::Notif_Trap),
                           ui::theme::NotifyPriority::High);

            // Тайник: докопавшемуся высыпается всё разом.
            if (treasures.update(*world, registry, ppos, worldSeed) > 0 && ui)
                ui->notify(cfg::T(cfg::StrKey::Notif_Treasure),
                           ui::theme::NotifyPriority::High);
        }
        if (stationSpawner && player) {
            const glm::vec3 ppos = player->controller.state().position;
            stationSpawner->update(registry, *world, ppos, worldSeed);
        }
        if (altarSpawner && player) {
            const glm::vec3 ppos = player->controller.state().position;
            altarSpawner->update(registry, *world, ppos, worldSeed);
        }

        combat::updateProjectiles(*world, registry, dt);
        combat::updateHitFx(registry, dt);
        combat::tickStatuses(registry, dt);

        progression::tickProgression(registry, dt);
        quests::tickQuestTime(registry, (u32)player->entity(), dt);

        locCheckTimer_ += dt;
        if (locCheckTimer_ >= 0.5f) {
            locCheckTimer_ = 0.f;
            auto p = player->controller.state().position;
            quests::notifyLocationReached(registry, (u32)player->entity(),
                                          { (i32)p.x, (i32)p.y, (i32)p.z });
        }

        // Phase 14: audio update
        {
            audio::AudioListener listener;
            listener.position = render->camera().position();
            listener.forward  = render->camera().forward();
            listener.up       = glm::vec3(0.f, 1.f, 0.f);
            audio::engine().update(dt, listener);

            musicCtx.paused = ui ? ui->paused() : false;

            // Бой и «под землёй» — обход пула мобов и вертикальный
            // столб чтений вокселей до потолка мира. Музыке хватает
            // четырёх раз в секунду; каждый кадр это было полторы
            // сотни захватов замков в пустоту.
            ambienceTimer_ += dt;
            if (ambienceTimer_ >= 0.25f) {
                ambienceTimer_ = 0.f;
                cachedHostiles_    = hostilesNearby();
                cachedUnderground_ = isUnderground(player->controller.state().position);
                if (world) {
                    const auto pp = player->controller.state().position;
                    cachedBiome_ = (u32)world->generator().biomeAt((i32)std::floor(pp.x),
                                                                   (i32)std::floor(pp.z));
                }
            }
            musicCtx.nearbyHostiles = cachedHostiles_;
            musicCtx.inCombat = musicCtx.nearbyHostiles > 0;

            // Деревня — та, в кольце которой стоим. Раньше здесь
            // стояло «рядом станция крафта», и деревенская музыка
            // включалась у любой наковальни посреди леса, а в самой
            // деревне молчала, стоило отойти от верстака.
            musicCtx.inVillage =
                player->villageStyle != world::VillageStyle::Count;
            musicCtx.villageStyle = (u32)player->villageStyle;

            // Биом под игроком. Читается вместе с боем и подземельем,
            // четыре раза в секунду: климат — четыре fBm-поля, каждый
            // кадр их считать незачем.
            musicCtx.biome = cachedBiome_;
            musicCtx.underground = cachedUnderground_;

            if (auto* hp = registry.get<ecs::Health>(player->entity()))
                musicCtx.playerHealthPct = hp->max > 0.f ? hp->current / hp->max : 1.f;
            musicDirector.update(dt, musicCtx);
        }

        // Режим раскладки живёт в UI, а перетаскивание — во вводе.
        touch.setLayoutMode(ui && ui->buttonLayoutMode &&
                            ui->screen == ui::Screen::Settings);

        // ТЗ 4.6: прогресс генерации мира вокруг игрока.
        // Считаем долю чанков в ближнем радиусе, у которых уже есть меш:
        // пока их мало, игрок смотрит в пустоту и без индикатора не
        // понимает, что происходит.
        // Индикатор загрузки: доля чанков в ближнем радиусе, у которых
        // уже есть данные. Восемь десятков поисков чанка, каждый —
        // разделяемый замок на общей карте, которую в это же время
        // пишут потоки генерации. Индикатору хватает четырёх раз в
        // секунду; каждый кадр он мешал тем, чей прогресс показывает.
        loadProgressTimer_ += dt;
        if (ui && loadProgressTimer_ >= 0.25f) {
            loadProgressTimer_ = 0.f;
            const i32 r = std::min(4, world->viewDistance());
            const glm::vec3 p = player->controller.state().position;
            const i32 pcx = (i32)std::floor(p.x / (f32)world::CHUNK_SIZE);
            const i32 pcz = (i32)std::floor(p.z / (f32)world::CHUNK_SIZE);

            u32 total = 0, ready = 0;
            for (i32 dz = -r; dz <= r; ++dz) {
                for (i32 dx = -r; dx <= r; ++dx) {
                    if (dx*dx + dz*dz > r*r) continue;
                    ++total;
                    auto c = world->findChunk(pcx + dx, pcz + dz);
                    if (c && c->generated.load(std::memory_order_acquire)) ++ready;
                }
            }
            ui->loadProgress = total ? (f32)ready / (f32)total : 1.f;
        }

        if (ui) ui->tickUi(dt);

        evJump_ = evBreak_ = evPlace_ = evAttack_ = evFinish_ =
            evInteract_ = evUseItem_ = evDash_ = evBlock_ = false;

        touch.endFrame();
    }

    void prepareFrame(f32 timeSec, f32 dt) {
        const auto now = std::chrono::steady_clock::now();
        const f32 elapsed = std::chrono::duration<f32>(now - fpsTime).count();
        ++frameCount;

        if (elapsed >= 1.0f) {
            fps = (f32)frameCount / elapsed;
            frameCount = 0;
            fpsTime = now;

            if (player && world) {
                auto* prog = player->progression();
                auto* inv = player->inventory();
                auto* wal = player->wallet();
                char timeBuf[32];
                playtime.format(timeBuf, sizeof(timeBuf));

                LOGI("FPS=%u LV=%u gold=%llu inv=%u mobs=%u npc=%u voices=%u cells=%zu time=%s",
                     (u32)fps,
                     prog ? prog->level : 0u,
                     (unsigned long long)(wal ? wal->gold : 0),
                     inv ? inv->usedSlotCount() : 0u,
                     spawner ? spawner->mobCount() : 0u,
                     npcSpawner ? npcSpawner->activeNpcCount() : 0u,
                     audio::engine().activeVoiceCount(),
                     spatialHash.cellCount(),
                     timeBuf);
            }
        }

        if (render && world && player) {
            const physics::RayHit hit = player->targetBlock(*world);
            render::precipInstances(precip, weather.snowMix(), precipInstances);
        render->setPrecip(precipInstances.data(), (u32)precipInstances.size());

        render->prepareFrame(vk, *world, registry, timeSec, dt,
                                 player.get(), hit, fps);
        }
    }
};

static int32_t handleInput(android_app* app, AInputEvent* e) {
    auto* eng = static_cast<Engine*>(app->userData);
    if (!eng || !eng->initialized) return 0;

    const i32 type   = AInputEvent_getType(e);
    const i32 source = AInputEvent_getSource(e);

    // ---- Стики геймпада (ТЗ 5.2, опционально) ----
    if (type == AINPUT_EVENT_TYPE_MOTION &&
        (source & AINPUT_SOURCE_CLASS_JOYSTICK)) {
        auto dead = [](f32 v) { return std::fabs(v) < 0.18f ? 0.f : v; };
        const f32 lx = dead(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0));
        const f32 ly = dead(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0));
        const f32 rx = dead(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0));
        const f32 ry = dead(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0));

        eng->touch.injectGamepadMove({ lx, -ly });   // экранный Y вниз
        // Правый стик масштабируем так, чтобы чувствительность совпадала
        // со свайпом: свайп нормирован на высоту экрана.
        eng->touch.injectGamepadLook({ rx * 0.02f, ry * 0.02f });
        return 1;
    }

    // ---- Касания ----
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        const i32 action  = AMotionEvent_getAction(e);
        const usize count = AMotionEvent_getPointerCount(e);
        const f32 t = (f32)AMotionEvent_getEventTime(e) / 1e9f;

        for (usize i = 0; i < count; ++i) {
            const i32 pid = AMotionEvent_getPointerId(e, i);
            const f32 x   = AMotionEvent_getX(e, i);
            const f32 y   = AMotionEvent_getY(e, i);
            eng->touch.onTouch(action, (i32)i, pid, x, y, t);
        }
        return 1;
    }

    // ---- Клавиши: кнопка «Назад» и кнопки геймпада ----
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const i32 action = AKeyEvent_getAction(e);
        const i32 key    = AKeyEvent_getKeyCode(e);
        const bool down  = (action == AKEY_EVENT_ACTION_DOWN);

        switch (key) {
            case AKEYCODE_BACK:
                // Назад закрывает открытый экран, а не приложение.
                if (down && eng->ui) eng->ui->onBackPressed();
                return 1;

            case AKEYCODE_BUTTON_A:
                eng->touch.injectGamepadButton(eng->btnJump_, down);
                return 1;
            case AKEYCODE_BUTTON_X:
                eng->touch.injectGamepadButton(eng->btnAttack_, down);
                return 1;
            case AKEYCODE_BUTTON_Y:
                eng->touch.injectGamepadButton(eng->btnInteract_, down);
                return 1;
            case AKEYCODE_BUTTON_B:
                eng->touch.injectGamepadButton(eng->btnUseItem_, down);
                return 1;
            case AKEYCODE_BUTTON_L1:
                eng->touch.injectGamepadButton(eng->btnBreak_, down);
                return 1;
            case AKEYCODE_BUTTON_R1:
                eng->touch.injectGamepadButton(eng->btnPlace_, down);
                return 1;
            case AKEYCODE_BUTTON_THUMBL:
                eng->touch.injectGamepadSprint(down);
                return 1;
            // Рывок на левом бампере: он под тем же пальцем, что и
            // стик направления, а рвутся туда, куда идут.
            case AKEYCODE_BUTTON_L2:
                eng->touch.injectGamepadButton(eng->btnDash_, down);
                return 1;
            // Защита — на правом курке, под тем же пальцем, что и
            // атака на X: блок и удар чередуют одной рукой.
            case AKEYCODE_BUTTON_R2:
                eng->touch.injectGamepadButton(eng->btnBlock_, down);
                return 1;
            case AKEYCODE_BUTTON_START:
                if (down && eng->ui) eng->ui->togglePause();
                return 1;
            default:
                break;
        }
    }
    return 0;
}

static void handleCmd(android_app* app, int32_t cmd) {
    auto* eng = static_cast<Engine*>(app->userData);
    if (!eng) return;

    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window) eng->onWindowInit(app);
            break;
        case APP_CMD_TERM_WINDOW:
            eng->onWindowTerm();
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED: {
            if (!app->window || !eng->initialized) break;
            const i32 ww = ANativeWindow_getWidth(app->window);
            const i32 wh = ANativeWindow_getHeight(app->window);
            eng->winW_ = ww; eng->winH_ = wh;
            eng->vk.onResize(app->window);
            eng->touch.setViewport(ww, wh);
            if (eng->ui) eng->ui->setScreenSize(ww, wh);
            eng->applySurfaceGeometry();
            break;
        }
        case APP_CMD_GAINED_FOCUS:
            eng->hasFocus = true;
            eng->updateRunning();
            break;
        case APP_CMD_LOST_FOCUS:
            if (eng->initialized && eng->player && eng->world) {
                eng->doSave(save::SaveManager::AUTOSAVE_PROFILE,
                            save::SaveManager::AUTOSAVE_SLOT);
            }
            eng->hasFocus = false;
            eng->updateRunning();
            break;
        case APP_CMD_LOW_MEMORY: {
            // Системе не хватает памяти. Больше всего её держит мир:
            // чанк — это четверть мегабайта вокселей, а их в круге
            // полторы сотни, то есть под сорок мегабайт, и это без
            // мешей в видеопамяти. Если не отдать ничего, система
            // выберет сама — и выберет весь процесс.
            //
            // Отдаём дальние чанки, оставляя ближний круг. Потоковая
            // загрузка вернёт их обратно по бюджету, когда память
            // появится, и по мере того, как игрок пойдёт в их сторону.
            if (!eng->initialized || !eng->world || !eng->player) {
                LOGW("APP_CMD_LOW_MEMORY (мир ещё не создан)");
                break;
            }
            constexpr i32 KEEP_CHUNKS = 3;   // ~96 блоков вокруг игрока
            const glm::vec3 pos = eng->player->controller.state().position;
            const usize before = eng->world->loadedChunks();
            auto doomed = eng->world->collectUnloadCandidates(pos, KEEP_CHUNKS);
            if (eng->render)
                for (const auto& c : doomed) eng->render->forgetChunk(c);
            eng->world->removeChunks(doomed);
            LOGW("APP_CMD_LOW_MEMORY: чанков было %zu, выгружено %zu, осталось %zu",
                 before, doomed.size(), eng->world->loadedChunks());
            break;
        }
        default:
            break;
    }
}

// ============================================================
// Точка входа. Вызывается из android_native_app_glue (код на C),
// поэтому имя не должно искажаться C++-манглингом.
// ============================================================
extern "C" void android_main(android_app* app) {
    // Первым делом — журнал: всё, что случится раньше, увидеть нельзя.
    crash::init(app->activity ? app->activity->internalDataPath : nullptr,
                app->activity ? app->activity->externalDataPath : nullptr);

    LOGI("================================================");
    LOGI(" VoxelRPG: android_main (Phase 1-15)");
#ifdef VOXEL_BUILD_SHA
    LOGI(" сборка: %s", VOXEL_BUILD_SHA);
#endif
    // Диагностический APK обязан объявляться сам, до всего остального:
    // иначе по журналу не отличить его от обычного, и снимок экрана
    // приписывается не тому режиму.
    if (cfg::DIAGNOSTIC_BUILD)
        LOGI(" debug_scene=true (build diagnostic mode)");
    LOGI("================================================");
    if (crash::logPath()[0])       LOGI("журнал: %s", crash::logPath());
    if (crash::sharedLogPath()[0]) LOGI("журнал (читается из Termux): %s",
                                        crash::sharedLogPath());

    crash::step("старт планировщика задач");
    jobs::gJobs.start();

    Engine eng;
    app->userData     = &eng;
    app->onAppCmd     = handleCmd;
    app->onInputEvent = handleInput;

    const auto startTime = std::chrono::steady_clock::now();
    auto lastTime = startTime;
    // Окно сводки измеряется НАСТОЯЩИМИ часами, а не накопленным dt.
    //
    // dt в диагностической сборке прибит к 1/60 (debug_scene живёт на
    // постоянном шаге), и «кадров в секунду» из него выходило
    // тождественно 60.0 при любой настоящей частоте: числитель и
    // знаменатель росли на один и тот же кадр. В журнале это выглядело
    // как ровно 60.0/с, тогда как игра шла за девяносто, и сводка
    // молча врала ровно там, где на неё и смотрят.
    auto statWall = std::chrono::steady_clock::now();
    u64 lastPresented = 0;
    u32 statReports = 0;
    f32 msUpdate = 0.f, msPrepare = 0.f, msDraw = 0.f;
    // «Рисование» само по себе не отвечает ни на один вопрос: внутри
    // него и ожидание экрана, и запись команд. Держим их врозь, а
    // рядом — время, которое GPU ДЕЙСТВИТЕЛЬНО рисовал, по его
    // собственным меткам.
    f32 msWait = 0.f, msRecord = 0.f, msSubmit = 0.f, msGpu = 0.f;
    u32 msFrames = 0;
    // Время GPU по проходам и число команд рисования в каждом.
    // Сумма проходов равна времени кадра по построению: проход
    // начинается там, где кончился предыдущий.
    f32 msPass[vk::Context::GPU_PASSES] = {};
    u32 drawPass[vk::Context::GPU_PASSES] = {};

    while (true) {
        // ALooper_pollAll помечен недоступным начиная с NDK r27: он мог
        // проглотить пробуждение. У ALooper_pollOnce есть отличие,
        // которое нельзя терять при замене: он возвращает
        // ALOOPER_POLL_CALLBACK, когда отработал колбэк дескриптора.
        // Это не конец очереди — опрос надо продолжать, а source при
        // этом не заполняется.
        for (;;) {
            // Таймаут пересчитывается на каждый опрос, и это существенно.
            // Пока рисовать нечего, ждём события бесконечно. Но готовность
            // окна наступает внутри обработки команды — прямо в этом цикле.
            // Со значением, вычисленным до цикла, следующий опрос снова
            // ждал бы бесконечно, и поток не доходил бы до отрисовки:
            // приложение запускалось, звучало и показывало пустой экран,
            // просыпаясь только на команды системы.
            const int timeoutMs = (eng.running && eng.initialized) ? 0 : -1;

            int events = 0;
            android_poll_source* source = nullptr;
            const int res = ALooper_pollOnce(timeoutMs, nullptr, &events,
                                             reinterpret_cast<void**>(&source));
            if (res < 0 && res != ALOOPER_POLL_CALLBACK) break;
            if (source) source->process(app, source);
            if (app->destroyRequested) {
                if (eng.initialized) eng.onWindowTerm();
                eng.releaseWorld();
                jobs::gJobs.stop();
                return;
            }
        }

        if (eng.wantQuit) {
            eng.wantQuit = false;
            if (eng.initialized) eng.onWindowTerm();
            eng.releaseWorld();
            jobs::gJobs.stop();
            ANativeActivity_finish(app->activity);
            return;
        }

        if (eng.running && eng.initialized) {
            const auto now = std::chrono::steady_clock::now();
            f32 dt = std::chrono::duration<f32>(now - lastTime).count();
            lastTime = now;
            if (dt > 0.1f) dt = 0.1f;
            if (dt < 0.f)  dt = 0.f;

            // Отладочная сцена живёт на постоянном шаге: настоящий dt
            // пляшет от загрузки телефона, и всё, что на него смотрит,
            // делало бы кадр разным от запуска к запуску.
            const bool dbgScene = cfg::settingsConst().debugScene;
            if (dbgScene) dt = world::SCENE_FIXED_DT;

            // Время отсчитывается от старта приложения: секунды от
            // эпохи (~1.75e9) во float дают шаг дискретизации ~128 с,
            // из-за чего ломался цикл дня и ночи и анимация в шейдерах.
            f32 timeSec = std::chrono::duration<f32>(now - startTime).count();
            // И на постоянном времени.
            if (dbgScene) timeSec = world::SCENE_TIME_SEC;

            // Покадровая раскладка по этапам. Без неё «15 кадров в
            // секунду» ничего не говорит: узкое место может быть в
            // логике, в записи команд или в ожидании GPU, и лечится
            // оно в каждом случае по-разному.
            const auto tA = std::chrono::steady_clock::now();
            eng.update(dt, timeSec);
            const auto tB = std::chrono::steady_clock::now();
            eng.prepareFrame(timeSec, dt);
            const auto tC = std::chrono::steady_clock::now();

            const auto tC1 = std::chrono::steady_clock::now();
            const bool haveFrame = eng.vk.beginFrame();
            const auto tC2 = std::chrono::steady_clock::now();
            if (!haveFrame) {
                // Цепочка устарела — обычно из-за поворота экрана.
                // Пересоздали её, значит обязаны заново перенести
                // размеры и угол в камеру и интерфейс.
                if (app->window) {
                    eng.vk.onResize(app->window);
                    eng.applySurfaceGeometry();
                }
            }
            // tC2..tC3 — запись команд, tC3..tD — отправка и показ.
            //
            // Держать их врозь пришлось после журнала с устройства:
            // «запись команд 10.5 мс» при шестидесяти трёх чанках и
            // 1884 индексах — это не запись. Число повторяло время
            // GPU (10.9 мс) кадр в кадр, а значит процессор стоял и
            // ждал GPU, и стоял он ВНУТРИ этого отрезка. Отрезок
            // накрывал и запись, и vkQueueSubmit с vkQueuePresentKHR,
            // то есть ровно те два места, где ожидание и живёт.
            auto tC3 = tC2;
            if (haveFrame) {
                if (eng.render) eng.render->render(eng.vk);
                tC3 = std::chrono::steady_clock::now();
                eng.vk.endFrame();
            }
            const auto tD = std::chrono::steady_clock::now();
            // tC1..tC2 — ожидание экрана, tC2..tD — запись команд.
            // Раньше и то и другое лежало в одном числе «рисование», а
            // это разные беды: первое означает, что мы упёрлись в
            // вертикальную синхронизацию или в GPU, второе — что
            // процессор не успевает записать кадр.

            auto ms = [](auto a, auto b) {
                return std::chrono::duration<f32, std::milli>(b - a).count();
            };
            msUpdate  += ms(tA, tB);
            msPrepare += ms(tB, tC);
            msDraw    += ms(tC, tD);
            msWait    += ms(tC1, tC2);
            msRecord  += ms(tC2, tC3);
            msSubmit  += ms(tC3, tD);
            msGpu     += eng.vk.lastGpuMs();
            // Развёртка ест ПОЛНОЕ время кадра: метка вокруг
            // командного буфера тайлеру не мешает, в отличие от
            // меток внутри прохода.
            if (eng.render && eng.render->passSweep().active() &&
                eng.render->passSweep().tick(dt, eng.vk.lastGpuMs())) {
                eng.render->passSweep().report();
                // Замер сделан — держать сборку открытой незачем, она
                // только греет телефон и портит следующий замер. Выход
                // идёт обычным путём, значит журнал по дороге попадает
                // в буфер обмена.
                if (cfg::settingsConst().exitAfterSweep) {
                    LOGI("развёртка закончена — выходим");
                    eng.wantQuit = true;
                }
            }
            for (u32 i = 0; i < vk::Context::GPU_PASSES; ++i) {
                msPass[i]   += eng.vk.passMs((vk::Context::GpuPass)i);
                drawPass[i] += eng.render
                             ? eng.render->stats().drawCalls[i] : 0u;
            }
            ++msFrames;

            // Сводка раз в три секунды. Без неё «чёрный экран» не
            // отличить от «кадры идут, но в них нечего показать»:
            // по числу показанных кадров, положению камеры и числу
            // нарисованных чанков видно, какая именно это беда.
            // Первые отчёты — каждую секунду: приложение могут свернуть
            // через пару секунд, и редкая сводка ничего не успеет сказать.
            const f32 statSec =
                std::chrono::duration<f32>(now - statWall).count();
            if (statSec >= (statReports < 10 ? 1.f : 3.f)) {
                ++statReports;
                const u64 presented = eng.vk.framesPresented();
                const f32 fps = (f32)(presented - lastPresented) / statSec;
                lastPresented = presented;
                statWall = now;
                const glm::vec3 cam = eng.render ? eng.render->camera().position()
                                                 : glm::vec3(0.f);
                const f32 n = msFrames ? (f32)msFrames : 1.f;
                LOGI("кадры: показано %llu (%.1f/с), показ=%d, цепочка %llu | камера %.1f %.1f %.1f "
                     "| чанки: загружено %zu, нарисовано %u, индексов %u, "
                     "дыр %u (ждут меша %u) "
                     "| мобы %u, NPC %u, предметы %u, снаряды %u "
                     "| интерфейс: вершин %u, нарисовано %u, экран %d "
                     "| мс: логика %.1f, подготовка %.1f, рисование %.1f "
                     "(ожидание экрана %.1f, запись команд %.1f, "
                     "отправка и показ %.1f, GPU %s%.1f)",
                     (unsigned long long)presented, (double)fps,
                     (int)eng.vk.lastPresentResult(),
                     (unsigned long long)eng.vk.swapchainRebuilds(),
                     (double)cam.x, (double)cam.y, (double)cam.z,
                     eng.world ? eng.world->loadedChunks() : (usize)0,
                     eng.render ? eng.render->drawnChunks() : 0u,
                     eng.render ? eng.render->drawnIndices() : 0u,
                     eng.render ? eng.render->emptyChunks() : 0u,
                     eng.render ? eng.render->waitingChunks() : 0u,
                     eng.render ? eng.render->mobInstances() : 0u,
                     eng.render ? eng.render->npcInstances() : 0u,
                     // Выпавшие предметы и снаряды в сводке не
                     // печатались вовсе. А это кубы 0.35 блока,
                     // которые рисуются поверх уже готового ландшафта:
                     // по картинке сотня таких кубиков неотличима от
                     // артефакта рендера, по числу — сразу видна.
                     eng.render ? eng.render->itemInstances() : 0u,
                     eng.render ? eng.render->projInstances() : 0u,
                     eng.ui ? eng.ui->lastVertices() : 0u,
                     eng.ui ? eng.ui->lastDrawn() : 0u,
                     eng.ui ? (int)eng.ui->screen : -1,
                     (double)(msUpdate / n), (double)(msPrepare / n),
                     (double)(msDraw / n),
                     (double)(msWait / n), (double)(msRecord / n),
                     (double)(msSubmit / n),
                     eng.vk.gpuTimingAvailable() ? "" : "нет:",
                     (double)(msGpu / n));
                // Раскладка кадра по проходам. Отдельной строкой, а не
                // в общей: в одну она не влезает, а читают её тогда,
                // когда общая уже сказала «GPU занят» и остался вопрос
                // «чем именно».
                //
                // Мобильный GPU плиточный, и метка ВНУТРИ прохода
                // рендера показывает, когда GPU дошёл до этой точки в
                // потоке команд, а не сколько он красил именно этот
                // проход: фрагментная работа всех проходов перемешана
                // по плиткам. Поэтому числа годятся как порядок
                // величин и как сдвиг между сборками, но цену прохода
                // меряют вычитанием — render_passes в settings.cfg
                // выключает проход, и разность полного времени кадра
                // и есть его цена.
                {
                    char buf[512];
                    int off = std::snprintf(buf, sizeof(buf),
                        "проходы GPU (мс / команд): ");
                    for (u32 i = 0; i < vk::Context::GPU_PASSES && off > 0 &&
                                    off < (int)sizeof(buf); ++i) {
                        off += std::snprintf(buf + off, sizeof(buf) - (usize)off,
                            "%s %.2f/%u  ",
                            vk::Context::passName((vk::Context::GpuPass)i),
                            (double)(msPass[i] / n),
                            (unsigned)((f32)drawPass[i] / n + 0.5f));
                    }
                    LOGI("%s| чанки: рассмотрено %u, отсечено %u, "
                         "нарисовано %u, вершин %u, маска проходов 0x%02X",
                         buf,
                         eng.render ? eng.render->consideredChunks() : 0u,
                         eng.render ? eng.render->culledChunks() : 0u,
                         eng.render ? eng.render->drawnChunks() : 0u,
                         eng.render ? eng.render->drawnVertices() : 0u,
                         (unsigned)cfg::settingsConst().renderPasses);
                }

                msUpdate = msPrepare = msDraw = 0.f;
                msWait = msRecord = msSubmit = msGpu = 0.f;
                for (u32 i = 0; i < vk::Context::GPU_PASSES; ++i) {
                    msPass[i] = 0.f; drawPass[i] = 0;
                }
                msFrames = 0;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
}
