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

#include "core/log.h"
#include "core/job_system.h"
#include "core/math.h"

#include "config/settings.h"
#include "config/localization.h"
#include "config/playtime.h"

#include "input/touch.h"

#include "vk/vk_context.h"

#include "world/chunk_manager.h"
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
constexpr u32 DEFAULT_VIEW_DIST = 7;

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

        cfg::settings().load(settingsPath);
        cfg::L().setLanguage(cfg::settingsConst().language);

        if (!vk.init(w)) { LOGE("Vulkan init failed"); return; }

        const i32 ww = ANativeWindow_getWidth(w);
        const i32 wh = ANativeWindow_getHeight(w);

        saveMgr.init(internalDataPath.c_str());

        touch.setViewport(ww, wh);

        // Кнопки ещё не созданы — настройки ввода применятся в
        // setupButtons(), вместе с их раскладкой.

        world = std::make_unique<world::ChunkManager>(
            worldSeed, cfg::settingsConst().viewDistance);

        world->setBlockModifyCallback([this](i32 wx, i32 wy, i32 wz, u16 newId) {
            worldDelta.recordBlock(wx, wy, wz, newId);
        });

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

        initialized = true;
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

        running = false;

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
        initialized = false;
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

    /// Рядом ли идёт бой: есть ли враждебный моб, который нас
    /// преследует или атакует. Нужен музыкальному директору.
    bool combatNearby() const {
        if (!player) return false;
        const glm::vec3 ppos = player->controller.state().position;
        auto& reg = const_cast<ecs::Registry&>(registry);
        auto& pool = reg.pool<ecs::AIAgent>();
        for (usize i = 0; i < pool.size(); ++i) {
            const auto& agent = pool.at((u32)i);
            if (agent.state != ecs::AIAgent::Chase &&
                agent.state != ecs::AIAgent::Attack) continue;
            const ecs::Entity e = pool.entityAt((u32)i);
            if (!reg.has<mobs::MobTag>(e)) continue;
            auto* tf = reg.get<ecs::Transform>(e);
            if (!tf) continue;
            const glm::vec3 d = tf->position - ppos;
            if (glm::dot(d, d) < 32.f * 32.f) return true;
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

            player->updateWithHash(*world, &spatialHash, pin, dt,
                                   cameraYawPitch.x, cameraYawPitch.y);

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
            // В бою — если рядом есть враждебный моб в состоянии Chase/Attack.
            musicCtx.inCombat = combatNearby();
            musicCtx.inVillage = ui && ui->nearbyStation != crafting::StationType::None;
            musicDirector.update(dt, musicCtx);
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
            eng->running = eng->initialized;
            break;
        case APP_CMD_LOST_FOCUS:
            if (eng->initialized && eng->player && eng->world) {
                eng->doSave(save::SaveManager::AUTOSAVE_PROFILE,
                            save::SaveManager::AUTOSAVE_SLOT);
            }
            eng->running = false;
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
    LOGI("================================================");
    LOGI(" VoxelRPG: android_main (Phase 1-15)");
    LOGI("================================================");

    jobs::gJobs.start();

    Engine eng;
    app->userData     = &eng;
    app->onAppCmd     = handleCmd;
    app->onInputEvent = handleInput;

    const auto startTime = std::chrono::steady_clock::now();
    auto lastTime = startTime;

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int timeoutMs = eng.running ? 0 : -1;

        while (ALooper_pollAll(timeoutMs, nullptr, &events,
                               reinterpret_cast<void**>(&source)) >= 0) {
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
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
}
