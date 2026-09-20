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
#include "../items/item_use.h"
#include "../items/throwable.h"
#include "../quests/quest.h"
#include "../npc/npc_ai.h"
#include "../world/block.h"
#include "../audio/audio_events.h"
#include "../world/particles.h"
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
    // Утомление — только у игрока: мобы и жители не бегают марафонов
    // и не держат осаду, а лишний компонент в каждом из шести
    // десятков тел ничего не даёт.
    reg.add(entity_, ecs::Fatigue{});
    reg.add(entity_, ecs::Breath{ BREATH_SECONDS, BREATH_SECONDS, 0.f });

    ecs::Attributes attrs{};
    reg.add(entity_, attrs);


    ecs::Collider col;
    col.halfExtents = glm::vec3(0.30f, 0.90f, 0.30f);
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
    reg.add(entity_, combat::GuardState{});

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

    reg.add(entity_, progression::AttributeBuffs{});

    // Точка возвращения по умолчанию — там, где начали. Иначе первая
    // же смерть до ближайшего колодца уносила бы в нулевые координаты.
    ecs::Respawn rp;
    rp.point = spawnPos;
    rp.set   = true;
    reg.add(entity_, rp);

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

    // Исходные тиры — сразу, а не на первом кадре: между init() и
    // первым update() репутация уже может уехать (загрузка сейва).
    resyncReputationBaseline();
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

items::UseResult Player::useItem(world::ChunkManager& world, u32 slotIndex) {
    if (!reg_) return items::UseResult::Failed;

    // Метательное уходит своим путём: ему нужны мир и взгляд.
    auto* inv = reg_->get<items::Inventory>(entity_);
    if (inv && slotIndex < items::INV_TOTAL_SLOTS) {
        const auto& st = inv->at(slotIndex);
        if (!st.empty() &&
            items::items().get(st.itemId).category ==
                items::ItemCategory::Throwable)
        {
            return items::throwItemFromSlot(*reg_, world, entity_, slotIndex,
                                            eyePosition(), aimDir_);
        }
    }

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
    } else {
        // Отказ тоже надо услышать. Житель, который не станет
        // говорить с врагом деревни, иначе неотличим от промаха
        // пальцем: экран не меняется, звука нет.
        audio::events().uiError();
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

    // ---- Смерть и возвращение ----
    //
    // До всего остального: мёртвый не ходит, не бьёт и не собирает
    // предметы, а лежит. Дать ему ещё кадр обычной жизни значило бы
    // показать труп, идущий по своим делам.
    tickDeath(world, dt);
    if (dead) {
        controller.update(world, physics::MoveInput{}, dt);
        return;
    }

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

    // ---- Защита ----
    //
    // Поднимается ДО движения, потому что движение от неё зависит.
    //
    // Держать её можно только со свободными руками: замах уже начат —
    // значит, закрыться нечем. Отдельного «опустить защиту, чтобы
    // ударить» при этом не нужно: атака выводит оружие из Idle, и
    // защита падает сама. Так нажатие атаки остаётся мгновенным.
    auto* wsNow = reg_->get<combat::WeaponState>(entity_);
    const bool handsFree = !wsNow || wsNow->phase == combat::WeaponState::Idle;

    // Оглушение спрашиваем у КОМПОНЕНТА, а не у копии `statuses`:
    // копия снимается в конце кадра, а оглушают игрока между
    // кадрами — в апдейте тварей. Разница в один кадр здесь и есть
    // разница между «защита упала» и «защита упала, но ещё разок
    // сработала».
    const auto* seNow = reg_->get<combat::StatusEffects>(entity_);
    const bool stunned = seNow && seNow->stunned();

    const bool wantsGuard = input.blockHeld && !dialogueOpen && !dead &&
                            handsFree && !stunned;

    // Смотрит защита ТУДА, КУДА СМОТРИТ КАМЕРА, а не куда идут ноги:
    // ecs::Facing у игрока догоняет направление ходьбы, и отступая
    // спиной он держал бы щит в сторону отступления.
    const glm::vec3 guardDir { fwd.x, 0.f, fwd.z };
    bool guarding = false;
    if (auto* g = reg_->get<combat::GuardState>(entity_)) {
        g->tick(dt, wantsGuard, guardDir);
        guarding = g->up;
        // Копия для интерфейса снимается в конце кадра, вместе с
        // остальными: к тому времени по защите уже могли ударить.
    }

    // Со щитом наперевес не бегают. Это и есть постоянная цена
    // блока: держать его можно сколько угодно, но пока держишь — не
    // убежишь и не догонишь.
    if (guarding) wish *= combat::GUARD_SPEED_MULT;

    // ---- Оглушение ----
    //
    // Оглушённый игрок не ходит — ровно как оглушённая тварь. Раньше
    // стан на игроке не делал НИЧЕГО, кроме запрета атаковать:
    // накладывать его было некому, и это не замечалось. Пробитая
    // защита — первый в игре источник стана на игроке, и без этого
    // пробитие осталось бы словом без последствий.
    if (stunned) wish = glm::vec3(0.f);

    physics::MoveInput mi;
    mi.wishDir     = { wish.x, wish.z };
    mi.faceDir     = { fwd.x, fwd.z };
    mi.jumpPressed = input.jumpPressed && !dialogueOpen && !stunned;
    mi.jumpHeld    = input.jumpHeld && !dialogueOpen && !stunned;
    mi.crouch      = input.crouch;

    // ---- Бег ----
    //
    // Бег не стоил ничего: держи кнопку — и беги через весь мир.
    // Теперь он тратит выносливость, и тратит её только когда бег
    // ДЕЙСТВИТЕЛЬНО идёт: стоя на месте с зажатой кнопкой никто не
    // устаёт, а плывущему она и так не помогает.
    const bool wantsSprint = input.sprint && !dialogueOpen &&
                            !guarding && !stunned;
    const bool reallyRunning = wantsSprint && !winded_ &&
                               glm::length(mi.wishDir) > 0.1f &&
                               controller.state().onGround &&
                               !controller.state().inWater;
    if (reallyRunning)
        progression::consumeStamina(*reg_, entity_, SPRINT_STAMINA_PER_SEC * dt);
    if (auto* st = reg_->get<ecs::Stamina>(entity_)) {
        // Задохнулся — и не отдышится, пока не наберёт запас. Без
        // этого порога бег превращался бы в дрожь: выносливость
        // капает, кнопка снова срабатывает, и так каждый кадр.
        if (st->current <= 0.01f)               winded_ = true;
        else if (st->current >= SPRINT_RESUME)  winded_ = false;
    }
    mi.sprint = wantsSprint && !winded_;

    // ---- Рывок ----
    //
    // Выносливость снимается ЗДЕСЬ, а не в контроллере: физика не
    // знает ни о каких ресурсах, и знать не должна. Не хватило —
    // рывка просто не происходит, кнопка при этом ничего не тратит.
    // Рывок работает и из защиты: уйти перекатом из-под удара,
    // который нечем держать, — законный ответ, и отнимать его у
    // поднявшего щит незачем. Оглушённый не рвётся: на то и стан.
    mi.dashPressed = false;
    if (input.dashPressed && !dialogueOpen && !stunned &&
        controller.state().dashCooldown <= 0.f &&
        !controller.state().dashing())
    {
        if (progression::tryConsumeStamina(*reg_, entity_, DASH_STAMINA))
            mi.dashPressed = true;
    }

    // Запоминаем состояние до апдейта для детекта событий (land, jump).
    const bool wasOnGround = controller.state().onGround;
    const f32  prevVelY    = controller.state().velocity.y;

    // Phase 15: step-up/snap-down работают через controller.update.
    controller.update(world, mi, dt);

    // ---- Батут ----
    //
    // После движения, а не до: контроллер как раз обнулил падение,
    // приземлив игрока, — и подброс встаёт ровно на место посадки.
    // Горизонтальная скорость при этом сохраняется, поэтому батут и
    // разгоняет: с разбегу он кидает вперёд, а не только вверх.
    if (reg_) {
        const f32 bounce = items::trampolineBounceAt(
            *reg_, controller.state().position, controller.state().velocity.y);
        if (bounce > 0.f) {
            controller.state().velocity.y = bounce;
            controller.state().onGround   = false;
            audio::events().jump(controller.state().position);
        }
    }

    // ---- Детект событий после апдейта ----
    const bool isOnGround = controller.state().onGround;

    // Jump: если только что покинули землю и vy > 0
    if (wasOnGround && !isOnGround && controller.state().velocity.y > 0.5f) {
        audio::events().jump(controller.state().position);
    }

    // Land: если только что приземлились и до этого падали вниз
    if (!wasOnGround && isOnGround && prevVelY < -2.f) {
        const glm::vec3 feet = controller.state().position;
        audio::events().land(feet, prevVelY);

        // Пыль из-под ног — цветом ТОЙ ЗЕМЛИ, на которую упали.
        //
        // Приземление было единственным событием в игре, которое не
        // оставляло вообще ничего: ни звука мира, ни следа. Падение с
        // высоты и шаг со ступеньки выглядели одинаково.
        //
        // Проба на пять сантиметров ниже ступни, а не «блок под
        // floor(y)»: контроллер ставит ноги ровно на границу блоков,
        // и целая координата попадает то в опору, то в воздух над
        // ней.
        const i32 bx = (i32)std::floor(feet.x);
        const i32 by = (i32)std::floor(feet.y - 0.05f);
        const i32 bz = (i32)std::floor(feet.z);
        world::landingBurst(feet, world.getVoxel(bx, by, bz), -prevVelY);
    }

    // ---- Удар о землю ----
    //
    // Падать было не больно: с любой высоты игрок приземлялся целым,
    // и пропасть работала лифтом вниз. Обрыв без этого — не опасность,
    // а декорация.
    //
    // Считаем от ВЫСОТЫ полёта, а не от скорости удара: скорость
    // упирается в maxFallSpeed, и падение с двадцати блоков не
    // отличалось бы от падения с шестидесяти.
    lastFallDamage = 0.f;
    {
        const auto& st = controller.state();
        // Вода и лава сбрасывают отсчёт наравне с землёй, и это не
        // перестраховка: нырнувший с обрыва в озеро выплывает и
        // ВЫХОДИТ НА БЕРЕГ — то есть приземляется. Без сброса ему
        // засчитали бы падение, случившееся минуту назад и совсем в
        // другом месте.
        if (st.onGround || st.inWater || st.inLava) {
            if (!wasOnGround && isOnGround) {
                const f32 drop = fallPeakY_ - st.position.y;
                if (drop > FALL_SAFE_BLOCKS) {
                    combat::DamageInstance dmg;
                    dmg.amount = (drop - FALL_SAFE_BLOCKS) * FALL_DAMAGE_PER_BLOCK;
                    dmg.type   = combat::DamageType::Physical;
                    dmg.targetEntity = (u32)entity_;
                    dmg.sourceName   = "fall";
                    lastFallDamage = combat::applyDamage(*reg_, entity_, dmg);
                }
            }
            fallPeakY_ = st.position.y;
        } else {
            fallPeakY_ = std::max(fallPeakY_, st.position.y);
        }
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

            // Толчок камеры по ВЕСУ оружия. Кинжал едва задевает
            // кадр, топор бьёт заметно — оружие отличается не только
            // числом урона, но и тем, как отзывается экран.
            const auto& wd = combat::weapons().get(equipped.weaponId);
            cameraShake_ += 0.10f + std::min(0.20f, wd.knockback * 0.045f);
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
                lastAction.hitCount = (i32)hits.size();
                // Добивание — самый сильный удар в игре, и кадр
                // обязан это сказать.
                cameraShake_ += 0.55f;
            }
        }
    }

    // ---- Чем кончился удар по защите ----
    //
    // Звук и частицы у защиты свои и играются там же, где считается
    // сама защита. Сюда доходит только то, что знает один игрок:
    // насколько тряхнуть камеру и не пора ли объяснить парирование.
    if (auto* g = reg_->get<combat::GuardState>(entity_)) {
        const combat::GuardResult r = g->takeResult();
        if (r != combat::GuardResult::None) {
            switch (r) {
                case combat::GuardResult::Parried:
                    // Отбитый удар — лучшее, что может случиться в
                    // бою, и кадр обязан сказать это громче всего
                    // остального, кроме добивания.
                    cameraShake_ += 0.45f;
                    break;
                case combat::GuardResult::Broken:
                    cameraShake_ += 0.35f;
                    break;
                case combat::GuardResult::Blocked:
                    cameraShake_ += 0.16f;
                    // Заблокировал — значит, защита у него уже в
                    // руках, и про её вторую половину пора
                    // рассказать. До этого момента объяснять было
                    // нечего.
                    if (!parryHintShown_) {
                        parryHintShown_  = true;
                        pendingParryHint = true;
                    }
                    break;
                default: break;
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

    // ---- Смерть, возвращение и колодцы ----
    //
    // Смерть считается ДО движения: мёртвый не ходит, а лежит, и
    // дать ему ещё кадр ходьбы значило бы показать труп, идущий по
    // своим делам.
    newRespawnPoint = false;
    wellTimer_ += dt;
    if (wellTimer_ >= 0.5f) { wellTimer_ = 0.f; noticeWell(world); }

    enteredLair = false;
    enteredVillage = false;
    tickBreath(dt);
    lairTimer_ += dt;
    if (lairTimer_ >= 0.5f) {
        lairTimer_ = 0.f;
        noticeLair(world);
        noticeVillage(world);
    }

    // ---- Уведомление о смене тира репутации ----
    noticeReputationChange();
    if (reputationFlashTimer > 0.f) {
        reputationFlashTimer -= dt;
        if (reputationFlashTimer <= 0.f) reputationFlashActive = false;
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

    // Последним: к этому месту за кадр случилось всё, отчего бывает
    // больно, — удар твари, урон от падения, тик яда.
    noticeImpacts();
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

void Player::resyncReputationBaseline() {
    if (!reg_) return;
    auto* rep = reg_->get<factions::Reputation>(entity_);
    if (!rep) return;
    for (u8 i = 0; i < (u8)factions::FactionId::Count; ++i)
        prevRepTiers_[i] = rep->tier((factions::FactionId)i);
    repTiersKnown_ = true;
}

void Player::noticeWell(world::ChunkManager& world) {
    if (!reg_) return;
    auto* rp = reg_->get<ecs::Respawn>(entity_);
    if (!rp) return;

    const glm::vec3 p = controller.state().position;
    const i32 sx = (i32)std::floor(p.x / (f32)world::SUPER_CHUNK_BLOCKS);
    const i32 sz = (i32)std::floor(p.z / (f32)world::SUPER_CHUNK_BLOCKS);

    // Девять супер-чанков: деревня большая, её колодец может лежать
    // в соседней ячейке от той, в которой стоит игрок.
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const world::VillageSite v = world::villageAt(
                sx + dx, sz + dz, world.seed(), &world.generator());
            if (!v.exists) continue;

            const f32 ddx = (f32)v.center.x + 0.5f - p.x;
            const f32 ddz = (f32)v.center.z + 0.5f - p.z;
            if (ddx * ddx + ddz * ddz > WELL_TOUCH_DIST * WELL_TOUCH_DIST) continue;

            // Возвращаемся НА край колодца, а не в него: в середине
            // вода, и респаун туда был бы падением в воду.
            const glm::vec3 spot{ (f32)v.center.x + 2.5f,
                                  (f32)v.center.y + 1.f,
                                  (f32)v.center.z + 0.5f };
            const glm::vec3 d = spot - rp->point;
            if (glm::dot(d, d) < 1.f) return;   // этот уже запомнен

            rp->point = spot;
            rp->set   = true;
            newRespawnPoint = true;
            audio::events().uiClick();
            return;
        }
}

void Player::noticeLair(world::ChunkManager& world) {
    const glm::vec3 p = controller.state().position;
    const world::LairSite l = world::lairCovering(
        (i32)std::floor(p.x), (i32)std::floor(p.z),
        world.seed(), &world.generator());

    // Событие — это СМЕНА рода, а не нахождение внутри. Иначе
    // сообщение повторялось бы каждые полсекунды всё время, что
    // игрок стоит в логове.
    if (l.kind == lairKind) return;
    lairKind = l.kind;
    if (l.kind == world::LairKind::None) return;

    enteredLair = true;
    audio::events().uiClick();
}

void Player::tickBreath(f32 dt) {
    justDrowned = false;
    if (!reg_) return;
    auto* br = reg_->get<ecs::Breath>(entity_);
    if (!br) return;

    // Под водой — голова, а не ноги. По пояс в реке не задыхаются.
    if (!controller.state().submerged) {
        br->current = std::min(br->max,
            br->current + br->max * dt / BREATH_REFILL_SECONDS);
        br->chokeTimer = 0.f;
        return;
    }

    if (br->current > 0.f) {
        br->current = std::max(0.f, br->current - dt);
        return;
    }

    // Воздух кончился. Урон глотками, а не потоком: тонущий должен
    // успеть всплыть, а не умереть за полсекунды.
    br->chokeTimer += dt;
    if (br->chokeTimer < DROWN_INTERVAL) return;
    br->chokeTimer = 0.f;

    combat::DamageInstance dmg;
    dmg.amount       = DROWN_DAMAGE;
    dmg.type         = combat::DamageType::Physical;
    dmg.targetEntity = (u32)entity_;
    dmg.sourceName   = "drown";
    // Захлёб бьёт мимо неуязвимости после удара: иначе получивший по
    // голове тонул бы бесплатно.
    if (auto* hp = reg_->get<ecs::Health>(entity_)) hp->invulnTime = 0.f;
    combat::applyDamage(*reg_, entity_, dmg);
    justDrowned = true;
}

void Player::noticeVillage(world::ChunkManager& world) {
    const glm::vec3 p = controller.state().position;
    const i32 sx = (i32)std::floor(p.x / (f32)world::SUPER_CHUNK_BLOCKS);
    const i32 sz = (i32)std::floor(p.z / (f32)world::SUPER_CHUNK_BLOCKS);

    // Деревня считается «той, в которой стоим», в сорока блоках от
    // колодца: кольцо домов кончается на тридцати пяти, и за ним
    // деревня уже позади.
    constexpr f32 IN_VILLAGE = 40.f;

    world::VillageStyle found = world::VillageStyle::Count;
    for (i32 dz = -1; dz <= 1 && found == world::VillageStyle::Count; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const world::VillageSite v = world::villageAt(
                sx + dx, sz + dz, world.seed(), &world.generator());
            if (!v.exists) continue;
            const f32 ddx = (f32)v.center.x - p.x;
            const f32 ddz = (f32)v.center.z - p.z;
            if (ddx * ddx + ddz * ddz > IN_VILLAGE * IN_VILLAGE) continue;
            found = v.style;
            break;
        }

    // Событие — СМЕНА, а не нахождение: иначе игрок слушал бы про
    // деревню каждые полсекунды всё время, что по ней ходит.
    if (found == villageStyle) return;
    villageStyle = found;
    if (found == world::VillageStyle::Count) return;
    enteredVillage = true;
}

void Player::tickDeath(world::ChunkManager& world, f32 dt) {
    if (!reg_) return;
    auto* hp = reg_->get<ecs::Health>(entity_);
    if (!hp) return;

    justDied = justRespawned = false;

    if (!dead) {
        if (hp->current > 0.f) return;
        dead       = true;
        deathTimer = DEATH_DELAY;
        justDied   = true;
        return;
    }

    deathTimer -= dt;
    if (deathTimer > 0.f) return;

    // ---- Возвращение ----
    const auto* rp = reg_->get<ecs::Respawn>(entity_);
    glm::vec3 to = rp && rp->set ? rp->point : controller.state().position;

    // Если под точкой ничего нет — мир мог не догрузиться, — ставим
    // на поверхность по генератору: провалиться сквозь пол хуже, чем
    // появиться на метр выше.
    world::VoxelReader rd(world);
    if (rd.at((i32)std::floor(to.x), (i32)std::floor(to.y) - 1,
              (i32)std::floor(to.z)) == world::UNKNOWN)
    {
        to.y = (f32)world.generator().surfaceHeight((i32)std::floor(to.x),
                                                    (i32)std::floor(to.z)) + 1.f;
    }

    controller.setPosition(to);
    hp->current = hp->max;
    if (auto* m = reg_->get<ecs::Mana>(entity_))    m->current = m->max;
    if (auto* st = reg_->get<ecs::Stamina>(entity_)) st->current = st->max;
    // Вернувшийся возвращается ОТДОХНУВШИМ. Иначе смерть в затяжном
    // бою оставляла бы игрока вымотанным у колодца — наказание за
    // наказание.
    if (auto* fg = reg_->get<ecs::Fatigue>(entity_)) *fg = ecs::Fatigue{};
    if (auto* br = reg_->get<ecs::Breath>(entity_)) {
        br->current = br->max;
        br->chokeTimer = 0.f;
    }
    winded_ = false;
    if (auto* se = reg_->get<combat::StatusEffects>(entity_)) *se = {};

    dead          = false;
    deathTimer    = 0.f;
    justRespawned = true;
}

void Player::resyncImpactBaseline() {
    prevHealth_ = -1.f;
}

/// Накопить отдачу камеры от того, что случилось с игроком.
///
/// Считается по РАЗНИЦЕ здоровья, а не по перечню источников. Урон
/// приходит из пяти разных мест — тварь, яд, падение, утопление,
/// лава, — и шестое место, где они перечислены заново, разошлось бы
/// с ними при первом же новом источнике. Разница же верна для всех
/// сразу, включая те, которых ещё нет.
///
/// Заодно из этого само собой выходит нужное соотношение: тик яда
/// снимает доли единицы и кадр не трогает, удар Стража снимает
/// четверть полосы и бьёт по экрану заметно.
void Player::noticeImpacts() {
    if (!reg_) return;
    const auto* hp = reg_->get<ecs::Health>(entity_);
    if (!hp) return;

    const f32 now = hp->current;
    const f32 was = prevHealth_;
    prevHealth_ = now;

    if (was < 0.f) return;              // первый кадр или после загрузки
    const f32 taken = was - now;
    if (taken <= 0.f) return;           // лечение камеру не трясёт

    const f32 maxHp = hp->max > 1.f ? hp->max : 1.f;
    cameraShake_ += std::min(0.65f, taken / maxHp * 2.2f);
}

void Player::noticeReputationChange() {
    if (!reg_) return;
    auto* rep = reg_->get<factions::Reputation>(entity_);
    if (!rep) return;

    constexpr u8 N = (u8)factions::FactionId::Count;

    // Исходное состояние берётся при init() и после загрузки. Сюда
    // попадаем только если ни того, ни другого не было — тогда
    // запоминаем молча: объявлять «репутация стала нейтральной»
    // в момент появления на свет незачем.
    if (!repTiersKnown_) { resyncReputationBaseline(); return; }

    for (u8 i = 0; i < N; ++i) {
        const auto now = rep->tier((factions::FactionId)i);
        if (now == prevRepTiers_[i]) continue;
        prevRepTiers_[i] = now;

        // Экран показывает одну смену за раз. Если за кадр сменилось
        // несколько — покажется последняя; такое бывает только при
        // загрузке, и там уведомление не нужно вовсе.
        lastRepFaction        = (factions::FactionId)i;
        lastRepTier           = now;
        reputationFlashActive = true;
        reputationFlashTimer  = 2.0f;
    }
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
