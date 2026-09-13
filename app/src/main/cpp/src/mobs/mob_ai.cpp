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

glm::vec3 moveMob(world::ChunkManager& world,
                  const glm::vec3& pos,
                  const glm::vec3& vel,
                  f32 halfW, f32 height,
                  f32 dt)
{
    glm::vec3 np = pos;
    auto& reg = world::blocks();
    world::VoxelReader rd(world);

    auto blocked = [&](const glm::vec3& p) {
        for (f32 h = 0.1f; h < height; h += 0.4f) {
            i32 x = (i32)std::floor(p.x);
            i32 y = (i32)std::floor(p.y + h);
            i32 z = (i32)std::floor(p.z);
            if (reg.isSolid(rd.at(x, y, z))) return true;
        }
        return false;
    };

    glm::vec3 tryX = { np.x + vel.x * dt, np.y, np.z };
    if (!blocked(tryX)) np.x = tryX.x;

    glm::vec3 tryZ = { np.x, np.y, np.z + vel.z * dt };
    if (!blocked(tryZ)) np.z = tryZ.z;

    np.y += vel.y * dt;
    if (blocked(np)) np.y += 0.05f;
    return np;
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

void mobAttackPlayer(ecs::Registry& reg, ecs::Entity target, f32 damage)
{
    auto* h = reg.get<ecs::Health>(target);
    if (!h) return;

    const auto& d = progression::derivedOf(reg, target);
    if (d.dodgeChance > 0.f && combat::rollCritical(d.dodgeChance)) {
        return;
    }

    combat::DamageInstance dmg{};
    dmg.amount     = damage;
    dmg.type       = combat::DamageType::Physical;
    dmg.sourceEntity = 0;
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

void onMobDeath(ecs::Registry& reg, world::ChunkManager& world, ecs::Entity mob) {
    (void)reg; (void)world; (void)mob;
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
        i32 feetY = (i32)std::floor(pos.y);
        i32 fx = (i32)std::floor(pos.x), fz = (i32)std::floor(pos.z);
        u16 below = world.getVoxel(fx, feetY - 1, fz);
        u16 inside = world.getVoxel(fx, feetY, fz);
        ai->inWater = (inside == world::WATER) || (below == world::WATER);
        ai->onGround = (below != world::AIR) && (below != world::WATER);

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

        if (!ai->onGround && !ai->inWater) {
            vel->linear.y -= 22.f * dt;
            if (vel->linear.y < -50.f) vel->linear.y = -50.f;
        } else if (ai->onGround) {
            vel->linear.y = 0.f;
        } else if (ai->inWater) {
            vel->linear.y = 1.5f;
        }

        const bool isHostile = def.hostile;
        const f32 distToPlayer = glm::length((playerPos - pos));

        const bool wantFlee = isHostile &&
            (hp->current / def.maxHealth < FLEE_HEALTH_PCT) &&
            (agent->state == AIAgent::Chase || agent->state == AIAgent::Attack);

        if (wantFlee && agent->state != AIAgent::Flee) {
            agent->state = AIAgent::Flee;
            ai->stateTime = 0.f;
            ai->repathCooldown = 0.f;
        }

        if (stunned) {
            vel->linear.x *= std::exp(-8.f * dt);
            vel->linear.z *= std::exp(-8.f * dt);
            ai->walkPhase += dt * 1.f;
        } else {
            switch (agent->state) {
            case AIAgent::Idle: {
                vel->linear.x *= std::exp(-6.f * dt);
                vel->linear.z *= std::exp(-6.f * dt);
                ai->walkPhase += dt * 2.f;

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
                                     playerPos + glm::vec3(0, 1.f, 0))) {
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
                ai->walkPhase += dt * 6.f;

                if (isHostile && distToPlayer < def.aggroRange) {
                    if (hasLOS(world, pos + glm::vec3(0, def.eyeHeight, 0),
                                     playerPos + glm::vec3(0, 1.f, 0))) {
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
                    ai->attackAnim = 1.0f;
                    break;
                }
                if (distToPlayer > def.aggroRange * 1.6f) {
                    agent->state = AIAgent::Idle;
                    ai->stateTime = 0.f;
                    break;
                }

                if (ai->repathCooldown <= 0.f) {
                    repath(world, *ai, pos, playerPos);
                    ai->repathCooldown = REPATH_INTERVAL * 0.6f;
                }
                bool end = false;
                glm::vec3 dir = followPath(*ai, pos, end);
                if (end) {
                    glm::vec3 d = playerPos - pos; d.y = 0;
                    if (glm::length(d) > 0.1f) dir = glm::normalize(d);
                }
                vel->linear.x = dir.x * def.chaseSpeed * speedMult;
                vel->linear.z = dir.z * def.chaseSpeed * speedMult;
                ai->walkPhase += dt * 9.f;

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
                if (ai->attackAnim > 0.f)
                    ai->attackAnim = std::max(0.f, ai->attackAnim - dt * 3.f);

                // ---- Босс: смена фазы по порогу здоровья ----
                f32 dmgMult   = 1.f;
                f32 cooldown  = 1.2f;
                if (def.isBoss) {
                    const u8 phase = bossPhaseFor(hp->current / def.maxHealth,
                                                  def.phaseCount);
                    if (phase > ai->bossPhase) {
                        ai->bossPhase = phase;
                        ai->phaseRoarTimer = 1.2f;   // пауза на переход
                        ai->attackAnim = 1.0f;
                        LOGI("Босс %s переходит в фазу %u", def.name, (u32)phase + 1);
                    }
                    // На последней фазе — ярость: быстрее и больнее.
                    if (ai->bossPhase + 1 >= def.phaseCount) {
                        dmgMult  = def.enrageMult;
                        cooldown = 1.2f / def.enrageMult;
                    }
                }

                if (ai->phaseRoarTimer > 0.f) {
                    ai->phaseRoarTimer -= dt;
                    break;   // во время перехода босс не бьёт
                }

                if (ai->attackCooldown <= 0.f) {
                    // Со второй фазы каждый третий удар — по площади.
                    const bool canSlam = def.isBoss && def.slamRadius > 0.f
                                      && ai->bossPhase >= 1
                                      && ai->slamCounter >= 2;
                    if (canSlam) {
                        ai->slamCounter = 0;
                        if (distToPlayer < def.slamRadius) {
                            mobAttackPlayer(reg, playerEntity,
                                            def.slamDamage * dmgMult);
                        }
                        ai->attackAnim = 1.0f;
                        cooldown *= 1.6f;   // мощная атака дольше откатывается
                    } else if (distToPlayer < def.attackRange + 0.4f) {
                        mobAttackPlayer(reg, playerEntity,
                                        def.attackDamage * dmgMult);
                        if (def.isBoss) ++ai->slamCounter;
                    }
                    ai->attackCooldown = cooldown;
                    ai->attackAnim = 1.0f;
                }

                if (ai->stateTime > 1.5f) {
                    if (distToPlayer > def.attackRange) {
                        agent->state = AIAgent::Chase;
                        ai->stateTime = 0.f;
                        ai->repathCooldown = 0.f;
                    } else {
                        ai->stateTime = 0.f;
                    }
                }
                break;
            }
            case AIAgent::Flee: {
                glm::vec3 away = pos - playerPos; away.y = 0;
                f32 d = glm::length(away);
                if (d > 0.1f) away /= d;
                vel->linear.x = away.x * def.chaseSpeed * speedMult;
                vel->linear.z = away.z * def.chaseSpeed * speedMult;
                ai->walkPhase += dt * 9.f;

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

        glm::vec3 newPos = moveMob(world, pos, vel->linear,
                                   def.bodyRadius, def.bodyHeight, dt);
        tf->position = newPos;

        if (newPos.y < -10.f) toRemove.push_back(e);
    }

    for (ecs::Entity e : toRemove) {
        reg.destroy(e);
    }
}

} // namespace mobs
