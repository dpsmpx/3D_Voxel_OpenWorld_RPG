// ============================================================
// src/main.cpp — точка входа VoxelRPG. Фазы 1–15.
// ============================================================

#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>

#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>
#include <thread>
#include <ctime>
#include <string>
#include <algorithm>
#include <atomic>

#include "core/log.h"
#include "core/job_system.h"
#include "core/math.h"

#include "config/settings.h"
#include "config/localization.h"
#include "config/playtime.h"

#include "input/touch.h"

#include "vk/vk_context.h"

#include "world/chunk_manager.h"
#include "world/block.h"
#include "world/enchant_altar.h"
#include "world/day_cycle.h"

#include "render/render_system.h"

#include "player/player.h"
#include "physics/raycast.h"
#include "physics/collision.h"

#include "mobs/spawner.h"
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

using namespace ecs;
namespace cfg = config;

constexpr u64 DEFAULT_SEED      = 0xC0FFEEULL;

struct Engine {
    vk::Context                            vk;
    input::TouchInput                      touch;
    ecs::Registry                          registry;
    std::unique_ptr<world::ChunkManager>   world;
    std::unique_ptr<render::RenderSystem>  render;
    std::unique_ptr<player::Player>        player;
    std::unique_ptr<ui::UiSystem>          ui;
    std::unique_ptr<mobs::Spawner>         spawner;
    std::unique_ptr<npc::NpcSpawner>       npcSpawner;
    std::unique_ptr<crafting::StationSpawner>  stationSpawner;
    std::unique_ptr<world::EnchantAltarSpawner> altarSpawner;

    save::SaveManager                      saveMgr;
    save::WorldDeltaStore                  worldDelta;

    // Phase 14/15: audio
    audio::MusicDirector                   musicDirector;
    audio::MusicContext                    musicCtx;
    f32                                    footstepTimer = 0.f;

    // Phase 15: spatial hash
    combat::SpatialHash                    spatialHash;

    u64  worldSeed      = DEFAULT_SEED;

    /// Игровые сутки: спавн мобов, солнце, цвет неба, ассортимент
    /// торговцев. Сохраняются вместе с миром.
    world::DayCycle                        dayCycle;

    cfg::PlaytimeTracker playtime;
    f32  autosaveTimer  = 0.f;

    u32  lastSaveProfile = 0;
    u32  lastSaveSlot    = 0;

    std::string settingsPath;
    std::string internalDataPath;

    glm::vec2 cameraYawPitch{ 0.f, -0.35f };
    glm::vec3 playerSpawn{ 4.5f, 60.f, 4.5f };

    u32 btnJump_    = 0;
    u32 btnBreak_   = 0;
    u32 btnPlace_   = 0;
    u32 btnCamera_  = 0;
    u32 btnSprint_  = 0;
    u32 btnAttack_  = 0;
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

    bool running     = false;
    bool initialized = false;

    /// Мир под игроком ещё генерируется — физику держим выключенной.
    bool waitingForGround = false;

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

        internalDataPath = app->activity->internalDataPath ?
                           app->activity->internalDataPath : "/tmp";
        settingsPath = internalDataPath + "/settings.cfg";

        crash::step("чтение настроек");
        cfg::settings().load(settingsPath);
        cfg::L().setLanguage(cfg::settingsConst().language);

        crash::step("Vulkan: создание контекста");
        if (!vk.init(w)) { LOGE("Vulkan init failed"); return; }

        const i32 ww = ANativeWindow_getWidth(w);
        const i32 wh = ANativeWindow_getHeight(w);

        crash::step("менеджер сохранений");
        saveMgr.init(internalDataPath.c_str());

        touch.setViewport(ww, wh);

        // Кнопки ещё не созданы — настройки ввода применятся в
        // setupButtons(), вместе с их раскладкой.

        crash::step("менеджер чанков");
        world = std::make_unique<world::ChunkManager>(
            worldSeed, cfg::settingsConst().viewDistance);

        world->setBlockModifyCallback([this](i32 wx, i32 wy, i32 wz, u16 newId) {
            worldDelta.recordBlock(wx, wy, wz, newId);
        });

        crash::step("система рендера");
        render = std::make_unique<render::RenderSystem>();
        if (!render->init(vk, app->activity->assetManager)) {
            LOGE("RenderSystem init failed");
            return;
        }
        render->camera().setAspect((f32)ww / (f32)wh);
        render->camera().setViewport((u32)ww, (u32)wh);
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
        ui->setScreenSize(ww, wh);
        ui->showFps = cfg::settingsConst().showFps;

        ui->minimap.init(vk, 128);

        ui->onQuit = [this]() { wantQuit = true; };
        ui->onSave = [this]() { doSave(lastSaveProfile, lastSaveSlot); };
        ui->onSaveRequested = [this](u32 p, u32 s) { doSave(p, s); };
        ui->onLoadRequested = [this](u32 p, u32 s) { doLoad(p, s); };
        ui->onDeleteRequested = [this](u32 p, u32 s) {
            auto slot = saveMgr.slots().slot(p, s);
            slot.removeFiles();
            ui->refreshSlotMeta(saveMgr.slots());
            ui->setStatus(cfg::T(cfg::StrKey::Notif_Deleted));
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

            cfg::L().setLanguage(s.language);

            // Audio volumes
            audio::engine().setMasterVolume(s.masterVolume);
            audio::engine().setMusicVolume(s.musicVolume);
            audio::engine().setSfxVolume(s.sfxVolume);

            s.save(settingsPath);
        };

        ui->onUseItem = [this](u32 slotIndex) {
            if (!player) return;
            auto res = player->useItem(slotIndex);
            if (res == items::UseResult::Equipped) {
                audio::events().uiClick();
                ui->setStatus("Equipped");
            } else if (res == items::UseResult::NoEffect) {
                ui->setStatus("No effect");
            } else if (res == items::UseResult::Consumed) {
                ui->setStatus("Consumed");
            }
        };

        ui->onEquipHotbar = [this](u32 slotIndex) {
            if (!player) return;
            u8 i = (u8)(slotIndex - items::INV_HOTBAR_OFFSET);
            player->setActiveHotbar(i);
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

            const auto& r = crafting::recipes().get((u16)recipeId);
            auto st = crafting::craft(ctx, r);
            if (st == crafting::CraftStatus::Ok) {
                audio::events().craft();
                ui->setStatus(cfg::T(cfg::StrKey::Notif_Crafted));
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

        const i32 surf = world->generator().surfaceHeight(4, 4);
        playerSpawn = { 4.5f, (f32)surf + 1.5f, 4.5f };
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
        }

        spawner        = std::make_unique<mobs::Spawner>();
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

        fpsTime = std::chrono::steady_clock::now();

        crash::step("инициализация завершена");
        initialized = true;
        updateRunning();
        running     = true;
        LOGI("=== VoxelRPG готов (Phase 15). Spawn y=%.1f ===", playerSpawn.y);
    }

    void onWindowTerm() {
        if (initialized && player && world) {
            LOGI("Автосейв при выходе...");
            doSave(save::SaveManager::AUTOSAVE_PROFILE,
                   save::SaveManager::AUTOSAVE_SLOT);
        }

        musicDirector.shutdown();
        audio::engine().shutdown();

        cfg::settings().save(settingsPath);

        // Окна больше нет — рисовать нельзя, чем бы ни был фокус.
        initialized = false;
        updateRunning();

        if (render) {
            render->setUiSystem(nullptr);
            render->shutdown();
            render.reset();
        }
        altarSpawner.reset();
        stationSpawner.reset();
        npcSpawner.reset();
        spawner.reset();
        ui.reset();
        player.reset();
        world.reset();

        vk.shutdown();
    }

    void doSave(u32 profile, u32 slot) {
        if (!player || !world) return;
        auto sl = saveMgr.slots().slot(profile, slot);

        auto st = saveMgr.save(sl, *world, registry,
                               player->entity(), worldDelta,
                               worldSeed, playtime.seconds(), dayCycle);

        if (st == save::SaveStatus::Ok) {
            lastSaveProfile = profile;
            lastSaveSlot    = slot;
            if (ui) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s P%u S%u",
                              cfg::T(cfg::StrKey::Notif_Saved),
                              profile + 1, slot + 1);
                ui->setStatus(buf);
                ui->refreshSlotMeta(saveMgr.slots());
            }
        } else {
            if (ui) ui->setStatus(save::statusString(st));
        }
    }

    void doLoad(u32 profile, u32 slot) {
        if (!player || !world) return;
        auto sl = saveMgr.slots().slot(profile, slot);

        u64 loadedSeed = worldSeed;
        u32 loadedPlaytime = 0;

        auto st = saveMgr.load(sl, *world, registry, player->entity(),
                               worldDelta, &loadedSeed, &loadedPlaytime,
                               &dayCycle);

        if (st == save::SaveStatus::Ok) {
            worldSeed = loadedSeed;
            playtime.set(loadedPlaytime);
            lastSaveProfile = profile;
            lastSaveSlot    = slot;

            if (auto* tf = registry.get<ecs::Transform>(player->entity())) {
                player->controller.setPosition(tf->position);
            }
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
    void setupButtons() {
        // Правый низ: атака и всё, что рядом с большим пальцем.
        btnAttack_ = touch.addButton({ 0.72f, -0.62f }, 100.f,
            [this](u32) { evAttack_ = true; }, nullptr);

        btnFinisher_ = touch.addButton({ 0.90f, -0.30f }, 60.f,
            [this](u32) { evFinish_ = true; }, nullptr);

        btnJump_ = touch.addButton({ 0.88f, -0.72f }, 85.f, nullptr, nullptr);

        // Левая половина: движение и контекстные действия.
        btnSprint_ = touch.addButton({ -0.88f, -0.20f }, 70.f, nullptr, nullptr);

        btnBreak_ = touch.addButton({ 0.52f, -0.30f }, 70.f,
            [this](u32) { evBreak_ = true; }, nullptr);

        btnPlace_ = touch.addButton({ 0.36f, -0.62f }, 80.f,
            [this](u32) { evPlace_ = true; }, nullptr);

        btnInteract_ = touch.addButton({ -0.52f, -0.62f }, 70.f,
            [this](u32) { evInteract_ = true; }, nullptr);

        btnUseItem_ = touch.addButton({ -0.36f, -0.86f }, 55.f,
            [this](u32) { evUseItem_ = true; }, nullptr);

        btnCamera_ = touch.addButton({ 0.92f, 0.62f }, 55.f,
            [this](u32) {
                if (!player) return;
                player->cameraMode =
                    (player->cameraMode == player::CameraMode::FirstPerson)
                        ? player::CameraMode::ThirdPerson
                        : player::CameraMode::FirstPerson;
            }, nullptr);

        buttonIds_[cfg::Btn_Attack]   = btnAttack_;
        buttonIds_[cfg::Btn_Finisher] = btnFinisher_;
        buttonIds_[cfg::Btn_Jump]     = btnJump_;
        buttonIds_[cfg::Btn_Sprint]   = btnSprint_;
        buttonIds_[cfg::Btn_Break]    = btnBreak_;
        buttonIds_[cfg::Btn_Place]    = btnPlace_;
        buttonIds_[cfg::Btn_Interact] = btnInteract_;
        buttonIds_[cfg::Btn_UseItem]  = btnUseItem_;
        buttonIds_[cfg::Btn_Camera]   = btnCamera_;

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

    void update(f32 dt, f32 timeSec) {
        if (!player || !world || !render) return;

        playtime.tick(dt);
        dayCycle.tick(dt);

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

        auto* dlg = player->activeDialogue();
        bool dialogueActive = dlg && dlg->active;
        if (ui) ui->setDialogueActive(dialogueActive);

        // Phase 15: обновляем spatial hash раз в кадр (лениво).
        spatialHash.tick();

        if (!uiBlockingInput) {
            const glm::vec2 camDelta = touch.cameraDelta();
            cameraYawPitch.x += camDelta.x * 3.0f;
            cameraYawPitch.y = glm::clamp(
                cameraYawPitch.y + camDelta.y * 3.0f, -1.5f, 1.5f);

            player::PlayerInput pin;
            pin.moveAxis      = touch.moveAxis();
            pin.jumpPressed   = evJump_;
            pin.jumpHeld      = touch.isButtonHeld(btnJump_);
            pin.sprint        = touch.isButtonHeld(btnSprint_);
            pin.attackPressed = evAttack_;
            pin.attackHeld    = touch.isButtonHeld(btnAttack_);
            pin.finisherInput = evFinish_;
            pin.interactPressed = evInteract_;

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
                    player->useItem(slot);
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
                }
            }

            world->update(player->controller.state().position);

            // Phase 15: обновление пикапов с raycast-коллизией.
            items::updatePickups(*world, registry, player->entity(),
                                 player->controller.state().position, dt);

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
                auto toUnload = world->collectUnloadCandidates(
                    player->controller.state().position);
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
        cam.setYawPitch(cameraYawPitch.x, cameraYawPitch.y);
        cam.setSunDir(dayCycle.sunDirection());
        cam.setSky(dayCycle.skyColor(), dayCycle.skyLight(), dayCycle.timeOfDay());
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

        static f32 locCheckTimer = 0.f;
        locCheckTimer += dt;
        if (locCheckTimer >= 0.5f) {
            locCheckTimer = 0.f;
            auto p = player->controller.state().position;
            quests::notifyLocationReached(registry, (u32)player->entity(),
                                          { (i32)p.x, (i32)p.y, (i32)p.z });
        }

        // Миникарта (раз в сек)
        if (ui && ui->minimap.size() > 0) {
            static f32 minimapTimer = 0.f;
            minimapTimer += dt;
            if (minimapTimer >= 1.0f) {
                minimapTimer = 0.f;
                ui->minimap.update(*world, player->controller.state().position, 64.f);
                ui->minimap.flushUpload(vk);
            }
        }

        // Phase 14: audio update
        {
            audio::AudioListener listener;
            listener.position = render->camera().position();
            listener.forward  = render->camera().forward();
            listener.up       = glm::vec3(0.f, 1.f, 0.f);
            audio::engine().update(dt, listener);

            musicCtx.paused = ui ? ui->paused() : false;

            // Бой — если рядом моб в состоянии Chase/Attack.
            musicCtx.nearbyHostiles = hostilesNearby();
            musicCtx.inCombat = musicCtx.nearbyHostiles > 0;

            // Деревня — рядом станция крафта или NPC.
            musicCtx.inVillage = ui && ui->nearbyStation != crafting::StationType::None;

            // Под землёй — если над головой есть перекрытие.
            const glm::vec3 ppos = player->controller.state().position;
            musicCtx.underground = isUnderground(ppos);

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
        if (ui) {
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
            evInteract_ = evUseItem_ = false;

        touch.endFrame();
    }

    void prepareFrame(f32 timeSec) {
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
            render->prepareFrame(vk, *world, registry, timeSec,
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
                eng->touch.injectGamepadButton(eng->btnSprint_, down);
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
            eng->vk.onResize(app->window);
            eng->touch.setViewport(ww, wh);
            if (eng->render) {
                eng->render->camera().setAspect((f32)ww / (f32)wh);
                eng->render->camera().setViewport((u32)ww, (u32)wh);
            }
            if (eng->ui) eng->ui->setScreenSize(ww, wh);
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
        case APP_CMD_LOW_MEMORY:
            LOGW("APP_CMD_LOW_MEMORY");
            break;
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
    f32 statTimer = 0.f;
    u64 lastPresented = 0;
    u32 statReports = 0;

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
                jobs::gJobs.stop();
                return;
            }
        }

        if (eng.wantQuit) {
            eng.wantQuit = false;
            if (eng.initialized) eng.onWindowTerm();
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

            // Время отсчитывается от старта приложения: секунды от
            // эпохи (~1.75e9) во float дают шаг дискретизации ~128 с,
            // из-за чего ломался цикл дня и ночи и анимация в шейдерах.
            const f32 timeSec = std::chrono::duration<f32>(now - startTime).count();

            eng.update(dt, timeSec);
            eng.prepareFrame(timeSec);

            if (!eng.vk.beginFrame()) {
                if (app->window) eng.vk.onResize(app->window);
            } else {
                if (eng.render) eng.render->render(eng.vk);
                eng.vk.endFrame();
            }

            // Сводка раз в три секунды. Без неё «чёрный экран» не
            // отличить от «кадры идут, но в них нечего показать»:
            // по числу показанных кадров, положению камеры и числу
            // нарисованных чанков видно, какая именно это беда.
            // Первые отчёты — каждую секунду: приложение могут свернуть
            // через пару секунд, и редкая сводка ничего не успеет сказать.
            statTimer += dt;
            if (statTimer >= (statReports < 10 ? 1.f : 3.f)) {
                ++statReports;
                const u64 presented = eng.vk.framesPresented();
                const f32 fps = (f32)(presented - lastPresented) / statTimer;
                lastPresented = presented;
                statTimer = 0.f;
                const glm::vec3 cam = eng.render ? eng.render->camera().position()
                                                 : glm::vec3(0.f);
                LOGI("кадры: показано %llu (%.1f/с), показ=%d | камера %.1f %.1f %.1f "
                     "| чанки: загружено %zu, нарисовано %u, индексов %u "
                     "| трава %u, мобы %u, NPC %u",
                     (unsigned long long)presented, (double)fps,
                     (int)eng.vk.lastPresentResult(),
                     (double)cam.x, (double)cam.y, (double)cam.z,
                     eng.world ? eng.world->loadedChunks() : (usize)0,
                     eng.render ? eng.render->drawnChunks() : 0u,
                     eng.render ? eng.render->drawnIndices() : 0u,
                     eng.render ? eng.render->grassCount() : 0u,
                     eng.render ? eng.render->mobInstances() : 0u,
                     eng.render ? eng.render->npcInstances() : 0u);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
}
