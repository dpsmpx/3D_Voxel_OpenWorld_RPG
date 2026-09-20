/**
 * @file mob_ai.cpp
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#include "mob_ai.h"
#include "mob_def.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../combat/status_effects.h"
#include "../progression/progression.h"
#include "../items/item_pickup.h"
#include "../items/loot_table.h"
#include "../world/block.h"
#include "../audio/audio_events.h"
#include "../core/log.h"
#include <cmath>
#include <random>
#include <algorithm>
#include <utility>
#include <vector>

namespace mobs {

using namespace ecs;

namespace {

constexpr f32 IDLE_TIME_MIN   = 1.5f;
constexpr f32 IDLE_TIME_MAX   = 4.0f;
constexpr f32 WANDER_RADIUS   = 8.f;
constexpr f32 REPATH_INTERVAL = 1.2f;
constexpr f32 FLEE_HEALTH_PCT = 0.30f;

std::mt19937& rng() {
    static std::mt19937 g(0xDEADBEEF);
    return g;
}

f32 frand(f32 lo, f32 hi) {
    std::uniform_real_distribution<f32> d(lo, hi);
    return d(rng());
}

/// Тело твари для движения. Составляется из её определения, чтобы
/// ширина и высота брались там же, где их назначили.
physics::CreatureBody bodyOf(const MobDef& def) {
    physics::CreatureBody b;
    b.halfWidth = def.bodyRadius;
    b.height    = def.bodyHeight;
    // Ступень в блок — базовая способность всех: без неё тварь
    // останавливается у первой же грядки. Мелочь вроде курицы
    // поднимается на свою высоту, не выше: курица, влезающая на
    // метровый уступ, выглядит нелепо.
    b.stepHeight = std::min(1.05f, std::max(0.6f, def.bodyHeight * 0.75f));
    b.jumpSpeed  = 7.0f;
    return b;
}

bool hasLOS(world::ChunkManager& world,
            const glm::vec3& from, const glm::vec3& to)
{
    glm::vec3 d = to - from;
    f32 dist = glm::length(d);
    if (dist < 0.001f) return true;
    d /= dist;

    auto& reg = world::blocks();
    world::VoxelReader rd(world);
    const f32 step = 0.4f;
    for (f32 t = 0.4f; t < dist; t += step) {
        glm::vec3 p = from + d * t;
        i32 x = (i32)std::floor(p.x);
        i32 y = (i32)std::floor(p.y);
        i32 z = (i32)std::floor(p.z);
        if (reg.isSolid(rd.at(x, y, z))) return false;
    }
    return true;
}

/// Сколько поисков пути разрешено в текущем кадре.
///
/// Один A* с бюджетом в полторы тысячи шагов — это тысячи обращений к
/// вокселям, а каждое берёт два замка и ищет чанк в таблице: единицы
/// миллисекунд. Когда в кадр попадали сразу несколько мобов, логика
/// съедала пятнадцать миллисекунд из шестнадцати. Мобу, которому не
/// хватило бюджета, ничего не делается: он идёт по старому пути и
/// пересчитает его в следующем кадре.
static i32 g_pathBudget = 0;

void resetPathBudget(i32 n) { g_pathBudget = n; }

void repath(world::ChunkManager& world,
            MobAI& ai,
            const glm::vec3& from, const glm::vec3& to)
{
    if (g_pathBudget <= 0) return;
    --g_pathBudget;

    world::ai::PathResult r = world::ai::findPath(
        world,
        { (i32)std::floor(from.x), (i32)std::floor(from.y), (i32)std::floor(from.z) },
        { (i32)std::floor(to.x),   (i32)std::floor(to.y),   (i32)std::floor(to.z) },
        ai.moveParams,
        1500);

    if (r.ok && r.waypoints.size() > 1) {
        world::ai::smoothPath(world, r.waypoints, ai.moveParams);
        ai.path = std::move(r.waypoints);
        ai.pathIndex = 1;
    } else {
        ai.path.clear();
        ai.pathIndex = 0;
    }
}

glm::vec3 followPath(const MobAI& ai, const glm::vec3& pos,
                     bool& reachedEnd)
{
    reachedEnd = false;
    if (ai.path.empty() || ai.pathIndex >= (i32)ai.path.size()) {
        reachedEnd = true;
        return {0,0,0};
    }
    glm::vec3 target = glm::vec3(ai.path[ai.pathIndex]) + glm::vec3(0.5f, 0.0f, 0.5f);
    glm::vec3 toT = target - pos;
    toT.y = 0.f;
    f32 d = glm::length(toT);
    if (d < 0.001f) return {0,0,0};
    return toT / d;
}

/// Кого тварь считает добычей.
///
/// Цель была ровно одна — игрок. Волк, забежавший в деревню, проходил
/// сквозь жителей насквозь, а стража рубила его, не получая сдачи:
/// «бой» был избиением в одну калитку и заканчивался всегда одинаково.
/// Теперь добычей считается и житель, и стражник.
struct Prey {
    ecs::Entity e{};
    glm::vec3   pos{0};
    f32         dist = 1e9f;
    bool valid() const { return e.valid(); }
};

Prey findPrey(ecs::Registry& reg,
              ecs::Entity playerEntity,
              const glm::vec3& playerPos,
              const glm::vec3& from,
              f32 maxDist)
{
    Prey best;
    const f32 maxD2 = maxDist * maxDist;

    auto consider = [&](ecs::Entity e, const glm::vec3& p) {
        const glm::vec3 d = p - from;
        const f32 d2 = glm::dot(d, d);
        if (d2 > maxD2 || d2 >= best.dist * best.dist) return;
        best.e = e; best.pos = p; best.dist = std::sqrt(d2);
    };

    if (playerEntity.valid()) {
        auto* hp = reg.get<ecs::Health>(playerEntity);
        if (hp && hp->current > 0.f) consider(playerEntity, playerPos);
    }

    // Жители. Их в деревне два десятка, а перебор идёт два раза в
    // секунду на тварь — это дешевле, чем кажется, и без него
    // деревню некому защищать ОТ КОГО.
    auto& npcs = reg.pool<ecs::NPCTag>();
    for (usize i = 0; i < npcs.size(); ++i) {
        const ecs::Entity e = npcs.entityAt((u32)i);
        auto* hp = reg.get<ecs::Health>(e);
        auto* tf = reg.get<ecs::Transform>(e);
        if (!hp || !tf || hp->current <= 0.f) continue;
        consider(e, tf->position);
    }
    return best;
}

/// Удар моба по цели.
///
/// `poisonDps`/`poisonTime` берутся из определения твари, а не из
/// ветки по её имени: «особая способность» — это свойство, и вторая
/// ядовитая тварь не должна требовать второй ветки.
void mobAttackPlayer(ecs::Registry& reg, ecs::Entity attacker,
                     ecs::Entity target, f32 damage,
                     f32 poisonDps = 0.f, f32 poisonTime = 0.f)
{
    auto* h = reg.get<ecs::Health>(target);
    if (!h || h->current <= 0.f) return;

    const auto& d = progression::derivedOf(reg, target);
    if (d.dodgeChance > 0.f && combat::rollCritical(d.dodgeChance)) {
        return;
    }

    combat::DamageInstance dmg{};
    dmg.amount     = damage;
    dmg.type       = poisonTime > 0.f ? combat::DamageType::Poison
                                      : combat::DamageType::Physical;
    dmg.poisonDps  = poisonDps;
    dmg.poisonTime = poisonTime;
    // Кто ударил — теперь известно. Ноль стоял здесь всегда, и
    // житель, которого грызёт волк, не мог узнать, кто его грызёт.
    dmg.sourceEntity = (u32)attacker;
    dmg.targetEntity = (u32)target;
    combat::applyDamage(reg, target, dmg);
}

} // namespace

void dealDamage(ecs::Registry& reg, ecs::Entity target, f32 dmg) {
    combat::DamageInstance d{};
    d.amount = dmg;
    d.type   = combat::DamageType::Physical;
    combat::applyDamage(reg, target, d);
}

void updateMobs(world::ChunkManager& world,
                ecs::Registry& reg,
                ecs::Entity playerEntity,
                const glm::vec3& playerPos,
                f32 dt)
{
    // Два поиска пути на кадр: этого хватает, чтобы стая не замирала,
    // и не хватает, чтобы уронить кадр.
    resetPathBudget(2);

    auto& pool = reg.pool<MobAI>();
    const usize n = pool.size();
    std::vector<ecs::Entity> toRemove;

    for (usize i = 0; i < n; ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* ai = pool.get(e);
        auto* tf = reg.get<Transform>(e);
        auto* vel = reg.get<Velocity>(e);
        auto* hp = reg.get<Health>(e);
        auto* tag = reg.get<MobTag>(e);
        auto* agent = reg.get<AIAgent>(e);
        auto* se = reg.get<combat::StatusEffects>(e);

        if (!ai || !tf || !vel || !hp || !tag || !agent) continue;

        const MobDef& def = mobRegistry().get(tag->id);
        if (tag->id == MOB_NONE) continue;

        if (hp->invulnTime > 0.f) hp->invulnTime = std::max(0.f, hp->invulnTime - dt);
        if (ai->damageFlash > 0.f) ai->damageFlash = std::max(0.f, ai->damageFlash - dt);
        if (ai->attackCooldown > 0.f) ai->attackCooldown -= dt;
        if (ai->repathCooldown > 0.f) ai->repathCooldown -= dt;
        ai->stateTime += dt;

        glm::vec3 pos = tf->position;
        const physics::CreatureBody body = bodyOf(def);
        // Опору и воду считает physics::stepCreature в конце кадра:
        // угадывать их по одному блоку под центром нельзя. Прежняя
        // догадка объявляла тварь стоящей, пока она падала сквозь
        // верхнюю половину блока, — скорость обнулялась, и тварь
        // зависала в воздухе на полблока над землёй.
        if (auto* lo = reg.get<ecs::Locomotion>(e)) lo->grounded = ai->onGround();
        if (ai->jumpCooldown > 0.f) ai->jumpCooldown -= dt;
        if (ai->targetCooldown > 0.f) ai->targetCooldown -= dt;

        if (agent->state == AIAgent::Dead) {
            ai->deathTimer += dt;

            // ---- Phase 12: дроп лута ----
            if (!ai->lootDropped) {
                ai->lootDropped = true;

                std::vector<items::ItemStack> drops;
                items::loot().get(tag->id).roll(drops, 1.0f);

                if (!drops.empty()) {
                    glm::vec3 dropPos = tf->position + glm::vec3(0.f, 0.6f, 0.f);
                    for (usize k = 0; k < drops.size(); ++k) {
                        const auto& s = drops[k];
                        if (s.empty()) continue;

                        // Детерминированный разброс
                        u32 seed1 = (u32)((e * 17u) ^ ((u32)s.itemId * 31u) ^ (u32)k);
                        u32 seed2 = (u32)((e * 23u) ^ ((u32)s.itemId * 37u) ^ (u32)k);

                        f32 rx = ((f32)(seed1 % 100) / 100.f - 0.5f) * 2.0f;
                        f32 rz = ((f32)(seed2 % 100) / 100.f - 0.5f) * 2.0f;
                        glm::vec3 v{ rx * 2.f, 3.0f + (f32)(seed1 % 100) / 100.f, rz * 2.f };

                        items::spawnPickup(reg, dropPos, s, v);
                    }
                }
            }

            if (ai->deathTimer > 1.6f) {
                toRemove.push_back(e);
            }
            vel->linear = glm::vec3(0);
            continue;
        }

        f32 speedMult = 1.f;
        bool stunned = false;
        if (se) {
            speedMult = se->speedMult();
            stunned = se->stunned();
            if (se->flashTimer > 0.f) {
                ai->damageFlash = std::max(ai->damageFlash, 0.15f);
            }
        }

        const bool isHostile = def.hostile;

        // ---- Кого преследуем ----
        //
        // Цель выбирается раз в полсекунды и живёт между кадрами:
        // иначе волк, бегущий к жителю, каждый кадр переобувался бы
        // на игрока и обратно и топтался бы между ними.
        glm::vec3 targetPos = playerPos;
        ecs::Entity targetEntity = playerEntity;
        if (isHostile) {
            // Сдача. Кто ударил — того и грызём: волк, которого рубит
            // стражник, разворачивался обратно к жителю и подставлял
            // спину. Теперь бой идёт в обе стороны.
            if (se && se->lastAttacker != 0) {
                const ecs::Entity atk = reg.fromId(se->lastAttacker);
                auto* ahp = reg.get<Health>(atk);
                auto* atf = reg.get<Transform>(atk);
                if (ahp && atf && ahp->current > 0.f && atk != e &&
                    glm::length(atf->position - pos) < def.aggroRange * 1.6f)
                {
                    ai->target = atk;
                    ai->targetCooldown = 1.5f;
                    if (agent->state == AIAgent::Idle ||
                        agent->state == AIAgent::Patrol)
                    {
                        agent->state = AIAgent::Chase;
                        ai->stateTime = 0.f;
                        ai->repathCooldown = 0.f;
                    }
                }
            }
            if (ai->targetCooldown <= 0.f) {
                const Prey p = findPrey(reg, playerEntity, playerPos, pos,
                                        def.aggroRange * 1.6f);
                ai->target = p.valid() ? p.e : ecs::Entity{};
                ai->targetCooldown = 0.5f;
            }
            if (ai->target.valid()) {
                auto* ttf = reg.get<Transform>(ai->target);
                auto* thp = reg.get<Health>(ai->target);
                if (ttf && thp && thp->current > 0.f) {
                    targetPos = ttf->position;
                    targetEntity = ai->target;
                } else {
                    ai->target = ecs::Entity{};
                }
            }
        }
        const f32 distToPlayer = glm::length(targetPos - pos);

        const bool wantFlee = isHostile &&
            (hp->current / def.maxHealth < FLEE_HEALTH_PCT) &&
            (agent->state == AIAgent::Chase || agent->state == AIAgent::Attack);

        if (wantFlee && agent->state != AIAgent::Flee) {
            agent->state = AIAgent::Flee;
            ai->stateTime = 0.f;
            ai->repathCooldown = 0.f;
        }

        // ---- Замах: ровно один тик за кадр ----
        //
        // Вне состояния Attack замаха быть не может: у сбежавшей или
        // отвлёкшейся твари рука не должна остаться занесённой, а
        // удар — прилететь из другого состояния. Оглушённый удара не
        // доводит тем более: иначе стан ничего не решал бы — тварь
        // замерла бы и всё равно ударила, когда замах дотикает.
        if (stunned || agent->state != AIAgent::Attack) ai->swing.cancel();
        const bool swingStruck = ai->swing.tick(dt);

        if (stunned) {
            vel->linear.x *= std::exp(-8.f * dt);
            vel->linear.z *= std::exp(-8.f * dt);
        } else {
            // ---- Самолечение ----
        //
        // Свойство твари, а не ветка по имени: у кого healPeriod
        // больше нуля, тот и лечится. Лечится, только когда ей и
        // правда плохо, — иначе полоска здоровья стояла бы полной
        // весь бой, и убить такую тварь было бы нельзя вовсе.
        if (def.healPeriod > 0.f && def.healAmount > 0.f) {
            ai->healCooldown -= dt;
            auto* mh = reg.get<ecs::Health>(e);
            if (mh && mh->current > 0.f && ai->healCooldown <= 0.f &&
                mh->current < mh->max * def.healBelow)
            {
                mh->current = std::min(mh->max, mh->current + def.healAmount);
                ai->healCooldown = def.healPeriod;
            }
        }

        switch (agent->state) {
            case AIAgent::Idle: {
                vel->linear.x *= std::exp(-6.f * dt);
                vel->linear.z *= std::exp(-6.f * dt);

                if (ai->stateTime > frand(IDLE_TIME_MIN, IDLE_TIME_MAX)) {
                    agent->state = AIAgent::Patrol;
                    ai->stateTime = 0.f;
                    f32 a = frand(0.f, 6.28318f);
                    f32 r = frand(3.f, WANDER_RADIUS);
                    ai->wanderTarget = ai->homePos +
                        glm::vec3(std::cos(a) * r, 0, std::sin(a) * r);
                    ai->repathCooldown = 0.f;
                }

                if (isHostile && distToPlayer < def.aggroRange) {
                    if (hasLOS(world, pos + glm::vec3(0, def.eyeHeight, 0),
                                     targetPos + glm::vec3(0, 1.f, 0))) {
                        agent->state = AIAgent::Chase;
                        ai->stateTime = 0.f;
                        ai->repathCooldown = 0.f;
                    }
                }
                break;
            }
            case AIAgent::Patrol: {
                if (ai->repathCooldown <= 0.f) {
                    repath(world, *ai, pos, ai->wanderTarget);
                    ai->repathCooldown = REPATH_INTERVAL;
                }
                bool end = false;
                glm::vec3 dir = followPath(*ai, pos, end);
                if (end || ai->stateTime > 8.f) {
                    agent->state = AIAgent::Idle;
                    ai->stateTime = 0.f;
                    break;
                }
                vel->linear.x = dir.x * def.walkSpeed * speedMult;
                vel->linear.z = dir.z * def.walkSpeed * speedMult;

                if (isHostile && distToPlayer < def.aggroRange) {
                    if (hasLOS(world, pos + glm::vec3(0, def.eyeHeight, 0),
                                     targetPos + glm::vec3(0, 1.f, 0))) {
                        agent->state = AIAgent::Chase;
                        ai->stateTime = 0.f;
                        ai->repathCooldown = 0.f;
                    }
                }
                if (!ai->path.empty() && ai->pathIndex < (i32)ai->path.size()) {
                    glm::vec3 wp = glm::vec3(ai->path[ai->pathIndex]) + glm::vec3(0.5f,0,0.5f);
                    glm::vec2 d2 { wp.x - pos.x, wp.z - pos.z };
                    if (glm::length(d2) < 0.4f) {
                        ai->pathIndex++;
                        if (ai->pathIndex >= (i32)ai->path.size()) {
                            agent->state = AIAgent::Idle;
                            ai->stateTime = 0.f;
                        }
                    }
                }
                break;
            }
            case AIAgent::Chase: {
                if (distToPlayer < def.attackRange) {
                    agent->state = AIAgent::Attack;
                    ai->stateTime = 0.f;
                    break;
                }
                if (distToPlayer > def.aggroRange * 1.6f) {
                    agent->state = AIAgent::Idle;
                    ai->stateTime = 0.f;
                    break;
                }

                if (ai->repathCooldown <= 0.f) {
                    repath(world, *ai, pos, targetPos);
                    ai->repathCooldown = REPATH_INTERVAL * 0.6f;
                }
                bool end = false;
                glm::vec3 dir = followPath(*ai, pos, end);
                if (end) {
                    glm::vec3 d = targetPos - pos; d.y = 0;
                    if (glm::length(d) > 0.1f) dir = glm::normalize(d);
                }
                vel->linear.x = dir.x * def.chaseSpeed * speedMult;
                vel->linear.z = dir.z * def.chaseSpeed * speedMult;

                if (!ai->path.empty() && ai->pathIndex < (i32)ai->path.size()) {
                    glm::vec3 wp = glm::vec3(ai->path[ai->pathIndex]) + glm::vec3(0.5f,0,0.5f);
                    glm::vec2 d2 { wp.x - pos.x, wp.z - pos.z };
                    if (glm::length(d2) < 0.5f) ++ai->pathIndex;
                }
                break;
            }
            case AIAgent::Attack: {
                vel->linear.x *= std::exp(-4.f * dt);
                vel->linear.z *= std::exp(-4.f * dt);

                // Смотреть надо на того, кого бьёшь. moveYaw при
                // остановке держит прежнее направление, и тварь,
                // остановившаяся у цели, замахивалась вполоборота:
                // по такому замаху не понять, куда придётся удар.
                if (auto* fc = reg.get<ecs::Facing>(e)) {
                    const glm::vec3 toTarget = targetPos - pos;
                    if (toTarget.x * toTarget.x + toTarget.z * toTarget.z > 1e-4f)
                        fc->moveYaw = std::atan2(toTarget.x, toTarget.z);
                }

                // ---- Босс: смена фазы по порогу здоровья ----
                f32 dmgMult   = 1.f;
                f32 cooldown  = 1.2f;
                f32 windup    = def.windupTime;
                if (def.isBoss) {
                    const u8 phase = bossPhaseFor(hp->current / def.maxHealth,
                                                  def.phaseCount);
                    if (phase > ai->bossPhase) {
                        ai->bossPhase = phase;
                        ai->phaseRoarTimer = 1.2f;   // пауза на переход
                        ai->swing.cancel();          // рёв обрывает замах
                        LOGI("Босс %s переходит в фазу %u", def.name, (u32)phase + 1);
                    }
                    // На последней фазе — ярость: быстрее и больнее.
                    // Замах укорачивается вместе с откатом: иначе
                    // «быстрее» значило бы только «чаще», а между
                    // ударами игрок имел бы прежний запас времени.
                    if (ai->bossPhase + 1 >= def.phaseCount) {
                        dmgMult  = def.enrageMult;
                        cooldown = 1.2f / def.enrageMult;
                        windup  /= def.enrageMult;
                    }
                }

                if (ai->phaseRoarTimer > 0.f) {
                    ai->phaseRoarTimer -= dt;
                    break;   // во время перехода босс не бьёт
                }

                // ---- Замах дошёл до цели ----
                //
                // Досягаемость проверяется ЗДЕСЬ, в момент попадания,
                // а не в момент начала замаха. В этом вся суть: шаг
                // назад или рывок за время замаха уводит из-под
                // удара, и тварь бьёт воздух. Раньше урон приходил в
                // тот же кадр, когда истекал откат, — отойти было
                // физически не от чего.
                if (swingStruck) {
                    const f32 reach = ai->swingIsSlam
                                    ? def.slamRadius
                                    : def.attackRange + 0.4f;
                    if (distToPlayer < reach) {
                        if (ai->swingIsSlam) {
                            mobAttackPlayer(reg, e, targetEntity,
                                            def.slamDamage * dmgMult);
                        } else {
                            mobAttackPlayer(reg, e, targetEntity,
                                            def.attackDamage * dmgMult,
                                            def.poisonDps, def.poisonTime);
                        }
                    } else {
                        // Промах слышно: свист вхолостую — это и есть
                        // награда за уклонение. Тяжёлое и лёгкое
                        // различаются по той же мерке, что у игрока,
                        // — по длине замаха.
                        if (windup > 0.40f) audio::events().swingHeavy(tf->position);
                        else                audio::events().swingLight(tf->position);
                    }
                    if (def.isBoss && !ai->swingIsSlam) ++ai->slamCounter;
                    ai->attackCooldown = cooldown *
                                         (ai->swingIsSlam ? 1.6f : 1.f);
                    ai->swingIsSlam = false;
                }

                // ---- Начало замаха ----
                if (!ai->swing.winding() && ai->attackCooldown <= 0.f) {
                    // Со второй фазы каждый третий удар — по площади.
                    const bool slam = def.isBoss && def.slamRadius > 0.f
                                   && ai->bossPhase >= 1
                                   && ai->slamCounter >= 2;
                    // Замахиваться на того, до кого заведомо не
                    // достаёшь, незачем: теперь промах означает, что
                    // цель УШЛА из-под удара, а не что тварь машет в
                    // пустоту от нечего делать.
                    const f32 reach = slam ? def.slamRadius
                                           : def.attackRange + 0.4f;
                    if (distToPlayer < reach) {
                        ai->swingIsSlam = slam;
                        if (slam) {
                            ai->slamCounter = 0;
                            // Большой удар объявляется крупно: иначе
                            // выйти из круга в пять метров нельзя, и
                            // «особая атака» отличалась бы только
                            // числом урона.
                            windup *= def.slamWindupMult;
                        }
                        ai->swing.begin(windup);
                        // Звук — В НАЧАЛЕ замаха, а не в момент урона.
                        // Раньше он приходил вместе с уроном, то есть
                        // не предупреждал ни о чём; теперь это
                        // единственное, что предупреждает об ударе со
                        // спины.
                        audio::events().mobAttack(tf->position);
                    }
                }

                // Не достаёт и не замахивается — значит, надо
                // догонять, а не стоять до истечения полутора секунд.
                // Пока замах идёт, тварь стои́т: удар начат, и
                // доводить его она обязана с места.
                if (!ai->swing.busy() && distToPlayer > def.attackRange) {
                    agent->state = AIAgent::Chase;
                    ai->stateTime = 0.f;
                    ai->repathCooldown = 0.f;
                } else if (ai->stateTime > 1.5f) {
                    ai->stateTime = 0.f;
                }
                break;
            }
            case AIAgent::Flee: {
                glm::vec3 away = pos - targetPos; away.y = 0;
                f32 d = glm::length(away);
                if (d > 0.1f) away /= d;
                vel->linear.x = away.x * def.chaseSpeed * speedMult;
                vel->linear.z = away.z * def.chaseSpeed * speedMult;

                if (hp->current / def.maxHealth > 0.7f || ai->stateTime > 8.f) {
                    agent->state = AIAgent::Idle;
                    ai->stateTime = 0.f;
                }
                break;
            }
            case AIAgent::Dead:
                break;
            }
        }

        // ---- Движение ----
        //
        // Тем же разрешением столкновений, каким ходит игрок:
        // ширина тела, потолок, подъём на ступень, притягивание к
        // земле при спуске. Своя копия «коллизии» у мобов проверяла
        // один столбец в центре и ширину тела не знала вовсе.
        const glm::vec2 wish{ vel->linear.x, vel->linear.z };
        const bool wantsMove = glm::dot(wish, wish) > 0.04f;
        physics::stepCreature(world, ai->motion, tf->position, vel->linear,
                              body, dt, wantsMove);

        // Упёрлись и не идём — прыжок. Ступенью берётся один блок,
        // прыжком — расщелина и забор в полтора. Без этого стая
        // толпилась у первой же изгороди, пока игрок смотрел.
        if (ai->motion.stuckTime > 0.6f && ai->jumpCooldown <= 0.f) {
            if (physics::tryJump(ai->motion, vel->linear, body)) {
                ai->jumpCooldown = 1.2f;
                ai->motion.stuckTime = 0.f;
            }
            // Заодно ищем дорогу заново: упор — это чаще всего
            // устаревший путь, а не забор.
            ai->repathCooldown = 0.f;
        }

        if (tf->position.y < -10.f) toRemove.push_back(e);
    }

    for (ecs::Entity e : toRemove) {
        reg.destroy(e);
    }
}

} // namespace mobs
