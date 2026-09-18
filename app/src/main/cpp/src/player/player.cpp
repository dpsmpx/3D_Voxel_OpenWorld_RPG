/**
 * @file player.cpp
 * @brief Игрок: ввод, движение, взаимодействие с миром и NPC.
 */
#include "player.h"
#include "player_rig.h"
#include "../combat/projectile.h"
#include "../ecs/components.h"
#include "../combat/status_effects.h"
#include "../combat/hit_detection.h"
#include "../progression/resource_regen.h"
#include "../items/item_pickup.h"
#include "../quests/quest.h"
#include "../npc/npc_ai.h"
#include "../world/block.h"
#include "../audio/audio_events.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace player {

// ============================================================
// init
// ============================================================
void Player::init(ecs::Registry& reg, const glm::vec3& spawnPos) {
    reg_ = &reg;
    entity_ = reg.create();

    ecs::Transform tf;
    tf.position = spawnPos;
    reg.add(entity_, tf);

    reg.add(entity_, ecs::Velocity{});

    // Ориентация и фаза шага — теми же компонентами, что у мобов и
    // NPC, и обновляются тем же проходом. У игрока не должно быть
    // своего способа поворачиваться: именно из таких «своих способов»
    // и вырастают расхождения.
    reg.add(entity_, ecs::Facing{ 0.f, 0.f, 12.f });
    reg.add(entity_, ecs::Gait{ 0.f, rig().strideLength });
    reg.add(entity_, ecs::Locomotion{});
    reg.add(entity_, ecs::Health{100.f, 100.f, 1.f, 0.f});
    reg.add(entity_, ecs::Mana{80.f, 80.f, 3.f});
    reg.add(entity_, ecs::Stamina{100.f, 100.f, 10.f});

    ecs::Attributes attrs{};
    reg.add(entity_, attrs);


    ecs::Collider col;
    col.halfExtents = glm::vec3(0.30f, 0.90f, 0.30f);
    col.isStatic = false;
    reg.add(entity_, col);

    reg.add(entity_, ecs::Kind{ecs::EntityKind::Player});
    reg.add(entity_, ecs::PlayerTag{});

    combat::Combatant cmb;
    cmb.faction  = combat::Faction::Player;
    cmb.radius   = 0.4f;
    cmb.height   = 1.8f;
    reg.add(entity_, cmb);

    combat::EquippedWeapon eq;
    eq.weaponId = combat::WEAPON_IRON_SWORD;
    eq.enchant.id = combat::EnchantmentId::None;
    reg.add(entity_, eq);

    reg.add(entity_, combat::WeaponState{});
    reg.add(entity_, combat::ResonanceState{});
    reg.add(entity_, combat::StatusEffects{});

    progression::Progression prog{};
    prog.level = 1;
    prog.availableAttrPoints = 0;
    prog.derivedDirty = true;
    reg.add(entity_, prog);

    progression::SkillTree tree{};
    reg.add(entity_, tree);

    quests::QuestLog qlog{};
    reg.add(entity_, qlog);

    factions::Reputation rep{};
    reg.add(entity_, rep);

    npc::ActiveDialogue dlg{};
    reg.add(entity_, dlg);

    items::Inventory inv{};
    reg.add(entity_, inv);

    items::Wallet wal{};
    wal.gold = 100;
    reg.add(entity_, wal);

    equipped     = eq;
    weaponState  = combat::WeaponState{};
    resonance    = combat::ResonanceState{};
    statuses     = combat::StatusEffects{};
    combatant    = cmb;

    controller.setPosition(spawnPos);
}

// ============================================================
// Accessors
// ============================================================
progression::Progression* Player::progression() {
    return reg_ ? reg_->get<progression::Progression>(entity_) : nullptr;
}
progression::SkillTree* Player::skillTree() {
    return reg_ ? reg_->get<progression::SkillTree>(entity_) : nullptr;
}
ecs::Attributes* Player::attributes() {
    return reg_ ? reg_->get<ecs::Attributes>(entity_) : nullptr;
}
quests::QuestLog* Player::questLog() {
    return reg_ ? reg_->get<quests::QuestLog>(entity_) : nullptr;
}
factions::Reputation* Player::reputation() {
    return reg_ ? reg_->get<factions::Reputation>(entity_) : nullptr;
}
npc::ActiveDialogue* Player::activeDialogue() {
    return reg_ ? reg_->get<npc::ActiveDialogue>(entity_) : nullptr;
}
items::Inventory* Player::inventory() {
    return reg_ ? reg_->get<items::Inventory>(entity_) : nullptr;
}
items::Wallet* Player::wallet() {
    return reg_ ? reg_->get<items::Wallet>(entity_) : nullptr;
}

const progression::DerivedStats& Player::derived() const {
    static progression::DerivedStats defaultStats{};
    if (!reg_) return defaultStats;
    auto* prog = reg_->get<progression::Progression>(entity_);
    if (!prog) return defaultStats;
    return prog->derived;
}

u16 Player::selectedBlock() const {
    if (!reg_) return legacySelectedBlock;
    auto* inv = reg_->get<items::Inventory>(entity_);
    if (!inv) return legacySelectedBlock;

    const auto& s = inv->activeSlot();
    if (s.empty()) return 0;

    const auto& def = items::items().get(s.itemId);
    if (def.category != items::ItemCategory::Block) return 0;
    return def.payload.blockId;
}

void Player::setActiveHotbar(u8 idx) {
    if (!reg_) return;
    auto* inv = reg_->get<items::Inventory>(entity_);
    if (!inv) return;
    if (idx >= items::INV_HOTBAR_SLOTS) return;
    inv->activeHotbar = idx;
}

items::UseResult Player::useItem(u32 slotIndex) {
    if (!reg_) return items::UseResult::Failed;
    auto res = items::useItemFromSlot(*reg_, entity_, slotIndex);

    if (auto* eq = reg_->get<combat::EquippedWeapon>(entity_)) equipped = *eq;
    return res;
}

bool Player::dropItem(u32 slotIndex) {
    if (!reg_) return false;
    glm::vec3 pos = controller.state().position + glm::vec3(0, 1.2f, 0);
    return items::dropItemFromSlot(*reg_, entity_, slotIndex, pos, aimDir_);
}

// ============================================================
// tryInteract
// ============================================================
void Player::tryInteract(ecs::Registry& reg, world::ChunkManager& world) {
    auto* dlg = reg.get<npc::ActiveDialogue>(entity_);
    if (!dlg) return;
    if (dlg->active) return;

    glm::vec3 pos = controller.state().position;
    ecs::Entity npcE = npc::findInteractableNpc(reg, pos, 3.5f);
    if (!npcE.valid()) return;

    auto* tag = reg.get<npc::NpcTag>(npcE);
    if (!tag) return;
    const npc::NpcDef& def = npc::npcRegistry().get(tag->id);

    if (npc::startDialogue(reg, world, (u32)entity_, (u32)npcE, def.dialogueRoot)) {
        if (auto* ai = reg.get<npc::NpcAI>(npcE)) {
            ai->inDialogue = true;
            ai->state = npc::NpcAI::Talk;
            ai->stateTime = 0.f;
        }
        // Phase 14: звук открытия диалога (используем UI-click).
        audio::events().uiClick();
    }
}

// ============================================================
// update / updateWithHash — thin wrappers над updateImpl
// ============================================================
void Player::update(world::ChunkManager& world,
                    const PlayerInput& input,
                    f32 dt,
                    f32 cameraYaw,
                    f32 cameraPitch)
{
    updateImpl(world, nullptr, input, dt, cameraYaw, cameraPitch);
}

void Player::updateWithHash(world::ChunkManager& world,
                            const combat::SpatialHash* hash,
                            const PlayerInput& input,
                            f32 dt,
                            f32 cameraYaw,
                            f32 cameraPitch)
{
    updateImpl(world, hash, input, dt, cameraYaw, cameraPitch);
}

// ============================================================
// updateImpl — основная логика
// ============================================================
void Player::updateImpl(world::ChunkManager& world,
                        const combat::SpatialHash* hash,
                        const PlayerInput& input,
                        f32 dt,
                        f32 cameraYaw,
                        f32 cameraPitch)
{
    if (!reg_) return;

    // ---- Производные характеристики → контроллер ----
    const auto& d = derived();
    controller.walkSpeed   = 4.5f * d.moveSpeedMult;
    controller.sprintSpeed = 7.5f * d.moveSpeedMult;
    controller.jumpVelocity = 9.0f * std::sqrt(std::max(0.1f, d.jumpHeightMult));

    // ---- Диалог блокирует движение / атаку ----
    auto* dlg = reg_->get<npc::ActiveDialogue>(entity_);
    const bool dialogueOpen = dlg && dlg->active;

    // ---- Направление движения ----
    const f32 cy = std::cos(cameraYaw);
    const f32 sy = std::sin(cameraYaw);
    const glm::vec3 fwd { sy, 0.f, cy };
    // Вектор «вправо» — это cross(fwd, up), ровно как его строит lookAt.
    // Здесь стояло (cos, 0, -sin), то есть ровно противоположное
    // направление, и шаг вбок уводил не в ту сторону.
    const glm::vec3 right { -cy, 0.f, sy };

    // moveAxis.y уже положителен «вперёд»: экранную ось джойстик
    // переворачивает у себя. Лишний минус здесь разворачивал ход назад.
    glm::vec3 wish = right * input.moveAxis.x + fwd * input.moveAxis.y;
    f32 len = glm::length(wish);
    if (len > 1.f) wish /= len;
    if (dialogueOpen) wish = glm::vec3(0);

    physics::MoveInput mi;
    mi.wishDir     = { wish.x, wish.z };
    mi.jumpPressed = input.jumpPressed && !dialogueOpen;
    mi.jumpHeld    = input.jumpHeld && !dialogueOpen;
    mi.sprint      = input.sprint;
    mi.crouch      = input.crouch;

    // Запоминаем состояние до апдейта для детекта событий (land, jump).
    const bool wasOnGround = controller.state().onGround;
    const f32  prevVelY    = controller.state().velocity.y;

    // Phase 15: step-up/snap-down работают через controller.update.
    controller.update(world, mi, dt);

    // ---- Детект событий после апдейта ----
    const bool isOnGround = controller.state().onGround;

    // Jump: если только что покинули землю и vy > 0
    if (wasOnGround && !isOnGround && controller.state().velocity.y > 0.5f) {
        audio::events().jump(controller.state().position);
    }

    // Land: если только что приземлились и до этого падали вниз
    if (!wasOnGround && isOnGround && prevVelY < -2.f) {
        audio::events().land(controller.state().position, prevVelY);
    }

    // ---- Направление прицела ----
    const f32 cp = std::cos(cameraPitch);
    const f32 sp = std::sin(cameraPitch);
    aimDir_ = { cp * sy, sp, cp * cy };

    // ---- Синхронизация ECS ----
    if (auto* tf = reg_->get<ecs::Transform>(entity_)) {
        tf->position = controller.state().position;
    }
    if (auto* lo = reg_->get<ecs::Locomotion>(entity_)) {
        // Опору знает контроллер. Угадывать её по вертикальной
        // скорости нельзя: в верхней точке прыжка она нулевая.
        lo->grounded = controller.state().onGround;
    }
    if (auto* v = reg_->get<ecs::Velocity>(entity_)) {
        v->linear = controller.state().velocity;
    }

    // ---- Interact ----
    if (input.interactPressed && !dialogueOpen) {
        tryInteract(*reg_, world);
    }

    // ---- Боевой апдейт ----
    if (!dialogueOpen) {
        combat::CombatInput ci;
        ci.attackPressed = input.attackPressed;
        ci.attackHeld    = input.attackHeld;
        ci.finisherInput = input.finisherInput;

        glm::vec3 attackOrigin =
            controller.state().position + glm::vec3(0.f, 1.3f, 0.f);

        // Phase 14: звук замаха в момент входа в Windup.
        // Проверяем фазу до вызова updateCombat.
        auto* wsBefore = reg_->get<combat::WeaponState>(entity_);
        const auto phaseBefore = wsBefore ? wsBefore->phase
                                          : combat::WeaponState::Idle;

        // Phase 15: combat получает spatial hash.
        combat::updateCombat(world, *reg_, hash, entity_,
                             attackOrigin, aimDir_, ci, dt, lastAction);

        auto* wsAfter = reg_->get<combat::WeaponState>(entity_);
        const auto phaseAfter = wsAfter ? wsAfter->phase
                                        : combat::WeaponState::Idle;

        if (phaseBefore == combat::WeaponState::Idle &&
            phaseAfter == combat::WeaponState::Windup)
        {
            // Определяем "лёгкое" / "тяжёлое" оружие.
            const auto& wdef = combat::weapons().get(equipped.weaponId);
            if (wdef.windupTime > 0.20f) {
                audio::events().swingHeavy(attackOrigin);
            } else {
                audio::events().swingLight(attackOrigin);
            }
        }

        // Phase 14: звук попадания.
        if (lastAction.didMeleeHit && lastAction.hitCount > 0) {
            audio::events().hitFlesh(lastAction.hitPoint);
        }

        if (lastAction.didShoot) {
            audio::events().arrowShoot(attackOrigin);
        }
        if (lastAction.didCastSpell) {
            audio::events().spellCast(attackOrigin, (u8)equipped.enchant.id);
        }

        // ---- Финишер ----
        if (input.finisherInput && resonance.finisherReady) {
            f32 mult = resonance.consumeFinisher();
            if (mult > 0.f) {
                mult *= d.finisherDamageMult;

                std::vector<combat::HitTarget> hits;
                combat::sphereHits(world, *reg_, hash, attackOrigin,
                                   5.5f,
                                   combat::Faction::Player,
                                   entity_,
                                   hits);

                const combat::WeaponDef& wdef =
                    combat::weapons().get(equipped.weaponId);
                for (const auto& h : hits) {
                    combat::DamageInstance dd{};
                    dd.amount       = wdef.baseDamage * mult * d.meleeDamageMult;
                    dd.type         = wdef.damageType;
                    dd.isCritical   = true;
                    dd.criticalMult = wdef.critMult + d.critDamageBonus;
                    dd.sourceEntity = (u32)entity_;
                    dd.targetEntity = (u32)h.entity;
                    combat::applyDamage(*reg_, h.entity, dd);

                    if (auto* v = reg_->get<ecs::Velocity>(h.entity)) {
                        glm::vec3 kd = h.center - attackOrigin;
                        kd.y = 0.f;
                        f32 l = glm::length(kd);
                        if (l > 0.01f) kd /= l;
                        v->linear += kd * 6.f;
                        v->linear.y += 3.5f;
                    }
                }
                combat::spawnHitFx(*reg_, attackOrigin,
                                   0xFFCC00FF, 0.6f, 3.5f, 0.45f);
                lastAction.didFinisher = true;
                lastAction.hitCount = (i32)hits.size();
            }
        }
    }

    // ---- Синхронизация ECS → self ----
    if (auto* ws = reg_->get<combat::WeaponState>(entity_))    weaponState = *ws;
    if (auto* rs = reg_->get<combat::ResonanceState>(entity_)) resonance = *rs;
    if (auto* se = reg_->get<combat::StatusEffects>(entity_))  statuses = *se;
    if (auto* eq = reg_->get<combat::EquippedWeapon>(entity_)) equipped = *eq;

    // ---- Level-up уведомление ----
    if (auto* prog = reg_->get<progression::Progression>(entity_)) {
        if (prog->pendingLevelUps > 0) {
            pendingLevelUpNotification = true;
            lastLevelGained = prog->level;
            levelUpFlashTimer = 2.0f;
            prog->pendingLevelUps = 0;

            // Phase 14: звук level-up.
            audio::events().levelUp();
        }
    }
    if (levelUpFlashTimer > 0.f) {
        levelUpFlashTimer -= dt;
        if (levelUpFlashTimer <= 0.f) pendingLevelUpNotification = false;
    }

    // ---- Head bob ----
    const f32 hs = glm::length(glm::vec2(controller.state().velocity.x,
                                         controller.state().velocity.z));
    if (controller.state().onGround && hs > 0.5f) {
        bobPhase += dt * (hs * 1.7f);
        bobAmount = std::min(1.f, hs / controller.sprintSpeed) * 0.05f;
    } else {
        bobAmount *= std::exp(-dt * 8.f);
    }
}

// ============================================================
// Пространственные функции
// ============================================================
glm::vec3 Player::eyePosition() const {
    return controller.state().position + glm::vec3(0.f, firstPersonEye, 0.f);
}

glm::vec3 Player::centerPosition() const {
    return controller.state().position +
           glm::vec3(0.f, controller.box.height * 0.5f, 0.f);
}

physics::RayHit Player::targetBlock(world::ChunkManager& world, f32 reach) const {
    glm::vec3 origin = eyePosition();
    return physics::raycastVoxels(world, origin, aimDir_, reach);
}

bool Player::tryBreakBlock(world::ChunkManager& world) {
    auto hit = targetBlock(world);
    if (!hit.hit) return false;
    if (hit.blockType == world::BEDROCK) return false;
    world.setVoxel(hit.block.x, hit.block.y, hit.block.z, world::AIR);

    // Блок уходил в никуда: воксель обращался в воздух, и на этом всё.
    // Таблица «блок → предмет» была построена при старте и ни разу не
    // спрошена — в игре, где мир состоит из блоков, добыча не давала
    // ничего. Роняем предмет на землю, а не кладём прямо в сумку: так
    // же поступают мобы, и полный инвентарь не съедает добытое молча.
    const u16 itemId = items::items().blockToItem(hit.blockType);
    if (itemId != items::ITEM_NONE && reg_) {
        const glm::vec3 centre{ (f32)hit.block.x + 0.5f,
                                (f32)hit.block.y + 0.5f,
                                (f32)hit.block.z + 0.5f };
        items::ItemStack drop;
        drop.itemId = itemId;
        drop.count  = 1;
        items::spawnPickup(*reg_, centre, drop, glm::vec3(0.f, 1.5f, 0.f));
    }
    return true;
}

bool Player::tryPlaceBlock(world::ChunkManager& world, u16 blockType) {
    if (blockType == 0) return false;
    auto hit = targetBlock(world);
    if (!hit.hit) return false;

    glm::ivec3 np = hit.block + hit.normal;
    if (np.y < 0 || np.y >= world::CHUNK_SIZE_Y) return false;
    if (world.getVoxel(np.x, np.y, np.z) != world::AIR) return false;

    const glm::vec3 pmin = controller.box.min(controller.state().position);
    const glm::vec3 pmax = controller.box.max(controller.state().position);
    const glm::vec3 bmin{ (f32)np.x,        (f32)np.y,        (f32)np.z };
    const glm::vec3 bmax{ (f32)np.x + 1.f,  (f32)np.y + 1.f,  (f32)np.z + 1.f };

    const bool overlap = !(bmax.x <= pmin.x || bmin.x >= pmax.x ||
                           bmax.y <= pmin.y || bmin.y >= pmax.y ||
                           bmax.z <= pmin.z || bmin.z >= pmax.z);
    if (overlap) return false;

    world.setVoxel(np.x, np.y, np.z, blockType);

    // Блок брался из ниоткуда: selectedBlock() читал активную ячейку
    // пояса и ничего из неё не списывал — стопка не таяла никогда.
    // Списываем ровно ту ячейку, из которой блок и был взят, и только
    // если она и правда держит этот блок.
    consumePlacedBlock(blockType);
    return true;
}

void Player::consumePlacedBlock(u16 blockType) {
    if (!reg_) return;
    auto* inv = reg_->get<items::Inventory>(entity_);
    if (!inv) return;

    auto& s = inv->activeSlot();
    if (s.empty()) return;

    const auto& def = items::items().get(s.itemId);
    if (def.category != items::ItemCategory::Block) return;
    if (def.payload.blockId != blockType) return;

    if (s.count > 1) --s.count;
    else             s.clear();
}

} // namespace player
