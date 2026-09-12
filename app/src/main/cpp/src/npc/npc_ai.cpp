/**
 * @file npc_ai.cpp
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#include "npc_ai.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../combat/status_effects.h"
#include "../combat/hit_detection.h"
#include "../world/block.h"
#include "../core/log.h"
#include <cmath>
#include <random>
#include <algorithm>
#include <vector>

namespace npc {

using namespace ecs;

namespace {

std::mt19937& rng() {
    static std::mt19937 g(0x11AA22BB);
    return g;
}

f32 frand(f32 lo, f32 hi) {
    return std::uniform_real_distribution<f32>(lo, hi)(rng());
}

constexpr f32 IDLE_MIN = 2.0f;
constexpr f32 IDLE_MAX = 5.0f;
constexpr f32 WANDER_RADIUS = 6.f;
glm::vec3 moveNpc(world::ChunkManager& world,
                  const glm::vec3& pos,
                  const glm::vec3& vel,
                  f32 halfW, f32 height,
                  f32 dt)
{
    glm::vec3 np = pos;
    auto& reg = world::blocks();

    auto blocked = [&](const glm::vec3& p) {
        for (f32 h = 0.1f; h < height; h += 0.4f) {
            i32 x = (i32)std::floor(p.x);
            i32 y = (i32)std::floor(p.y + h);
            i32 z = (i32)std::floor(p.z);
            if (reg.isSolid(world.getVoxel(x, y, z))) return true;
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
    const f32 step = 0.4f;
    for (f32 t = 0.4f; t < dist; t += step) {
        glm::vec3 p = from + d * t;
        i32 x = (i32)std::floor(p.x);
        i32 y = (i32)std::floor(p.y);
        i32 z = (i32)std::floor(p.z);
        if (reg.isSolid(world.getVoxel(x, y, z))) return false;
    }
    return true;
}

// Найти ближайшего врага (для Guard).
ecs::Entity findNearestEnemy(ecs::Registry& reg,
                             const glm::vec3& pos,
                             f32 maxDist)
{
    ecs::Entity best{};
    f32 bestD2 = maxDist * maxDist;

    auto& hp = reg.pool<ecs::Health>();
    for (usize i = 0; i < hp.size(); ++i) {
        ecs::Entity e = hp.entityAt((u32)i);
        if (!reg.has<ecs::EnemyTag>(e)) continue;
        auto* tf = reg.get<ecs::Transform>(e);
        if (!tf) continue;
        glm::vec3 d = tf->position - pos;
        d.y = 0.f;
        f32 d2 = glm::dot(d, d);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = e;
        }
    }
    return best;
}

void npcAttack(ecs::Registry& reg, ecs::Entity npc, ecs::Entity target,
               f32 damage)
{
    combat::DamageInstance d{};
    d.amount = damage;
    d.type   = combat::DamageType::Physical;
    d.sourceEntity = (u32)npc;
    d.targetEntity = (u32)target;
    combat::applyDamage(reg, target, d);
}

} // namespace

void updateNpcs(world::ChunkManager& world,
                ecs::Registry& reg,
                ecs::Entity playerEntity,
                const glm::vec3& playerPos,
                f32 dt)
{
    auto& pool = reg.pool<NpcAI>();
    const usize n = pool.size();
    std::vector<ecs::Entity> toRemove;

    for (usize i = 0; i < n; ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* ai = pool.get(e);
        auto* tf = reg.get<Transform>(e);
        auto* vel = reg.get<Velocity>(e);
        auto* hp = reg.get<Health>(e);
        auto* tag = reg.get<NpcTag>(e);
        if (!ai || !tf || !vel || !hp || !tag) continue;

        const NpcDef& def = npcRegistry().get(tag->id);
        if (tag->id == NPC_NONE) continue;

        // Таймеры
        if (hp->invulnTime > 0.f) hp->invulnTime = std::max(0.f, hp->invulnTime - dt);
        if (ai->damageFlash > 0.f) ai->damageFlash = std::max(0.f, ai->damageFlash - dt);
        if (ai->attackCooldown > 0.f) ai->attackCooldown -= dt;
        if (ai->offerCooldown > 0.f) ai->offerCooldown -= dt;
        ai->stateTime += dt;

        glm::vec3 pos = tf->position;
        i32 feetY = (i32)std::floor(pos.y);
        i32 fx = (i32)std::floor(pos.x), fz = (i32)std::floor(pos.z);
        u16 below = world.getVoxel(fx, feetY - 1, fz);
        bool onGround = (below != world::AIR) && (below != world::WATER);

        // Смерть
        if (ai->state == NpcAI::Dead || hp->current <= 0.f) {
            ai->state = NpcAI::Dead;
            ai->deathTimer += dt;
            if (ai->deathTimer > 3.0f) toRemove.push_back(e);
            vel->linear.x *= std::exp(-8.f * dt);
            vel->linear.z *= std::exp(-8.f * dt);
            continue;
        }

        // Гравитация
        if (!onGround) {
            vel->linear.y -= 22.f * dt;
            if (vel->linear.y < -50.f) vel->linear.y = -50.f;
        } else {
            vel->linear.y = 0.f;
        }

        // Диалог — NPC стоит
        if (ai->inDialogue) {
            vel->linear.x *= std::exp(-8.f * dt);
            vel->linear.z *= std::exp(-8.f * dt);
            ai->walkPhase += dt * 1.f;
            continue;
        }

        switch (ai->state) {

        case NpcAI::Idle: {
            vel->linear.x *= std::exp(-6.f * dt);
            vel->linear.z *= std::exp(-6.f * dt);
            ai->walkPhase += dt * 2.f;

            // Guard — сканирует врагов
            if (def.role == NpcRole::Guard && def.aggroRange > 0.f) {
                ecs::Entity enemy = findNearestEnemy(reg, pos, def.aggroRange);
                if (enemy.valid() &&
                    hasLOS(world, pos + glm::vec3(0, 1.2f, 0),
                                 [&]{ auto* t = reg.get<Transform>(enemy); return t ? t->position + glm::vec3(0, 1.f, 0) : pos; }()))
                {
                    ai->guardTarget = (u32)enemy;
                    ai->state = NpcAI::Combat;
                    ai->stateTime = 0.f;
                    break;
                }
            }

            if (ai->stateTime > frand(IDLE_MIN, IDLE_MAX)) {
                ai->state = NpcAI::Wander;
                ai->stateTime = 0.f;
                f32 a = frand(0.f, 6.28318f);
                f32 r = frand(2.f, WANDER_RADIUS);
                ai->wanderTarget = ai->homePos +
                    glm::vec3(std::cos(a) * r, 0, std::sin(a) * r);
            }
            break;
        }

        case NpcAI::Wander: {
            glm::vec3 d = ai->wanderTarget - pos;
            d.y = 0.f;
            f32 len = glm::length(d);
            if (len < 0.4f || ai->stateTime > 10.f) {
                ai->state = NpcAI::Idle;
                ai->stateTime = 0.f;
                break;
            }
            d /= len;
            vel->linear.x = d.x * def.moveSpeed;
            vel->linear.z = d.z * def.moveSpeed;
            ai->walkPhase += dt * 6.f;

            // Guard снова сканирует
            if (def.role == NpcRole::Guard && def.aggroRange > 0.f) {
                ecs::Entity enemy = findNearestEnemy(reg, pos, def.aggroRange);
                if (enemy.valid()) {
                    ai->guardTarget = (u32)enemy;
                    ai->state = NpcAI::Combat;
                    ai->stateTime = 0.f;
                }
            }
            break;
        }

        case NpcAI::Combat: {
            ecs::Entity target = (ecs::Entity)ai->guardTarget;
            auto* ttf = reg.get<Transform>(target);
            if (!ttf || !reg.get<Health>(target)) {
                ai->state = NpcAI::Idle;
                ai->guardTarget = 0;
                break;
            }
            glm::vec3 d = ttf->position - pos;
            d.y = 0.f;
            f32 dist = glm::length(d);
            if (dist > def.aggroRange * 1.8f) {
                ai->state = NpcAI::Idle;
                ai->guardTarget = 0;
                break;
            }
            if (dist < def.attackRange) {
                vel->linear.x *= std::exp(-4.f * dt);
                vel->linear.z *= std::exp(-4.f * dt);
                if (ai->attackCooldown <= 0.f) {
                    npcAttack(reg, e, target, def.attackDamage);
                    ai->attackCooldown = 1.4f;
                }
            } else {
                if (dist > 0.01f) d /= dist;
                vel->linear.x = d.x * def.moveSpeed;
                vel->linear.z = d.z * def.moveSpeed;
                ai->walkPhase += dt * 8.f;
            }
            break;
        }

        case NpcAI::Flee: {
            glm::vec3 away = pos - playerPos;
            away.y = 0.f;
            f32 len = glm::length(away);
            if (len > 0.1f) away /= len;
            vel->linear.x = away.x * def.moveSpeed * 1.5f;
            vel->linear.z = away.z * def.moveSpeed * 1.5f;
            ai->walkPhase += dt * 9.f;

            if (ai->stateTime > 6.f) {
                ai->state = NpcAI::Idle;
                ai->stateTime = 0.f;
            }
            break;
        }

        case NpcAI::Talk:
        case NpcAI::Follow:
        case NpcAI::Dead:
        default: break;
        }

        glm::vec3 np = moveNpc(world, pos, vel->linear,
                               def.bodyRadius, def.bodyHeight, dt);
        tf->position = np;

        if (np.y < -10.f) toRemove.push_back(e);
    }

    for (ecs::Entity e : toRemove) reg.destroy(e);
}

ecs::Entity findInteractableNpc(ecs::Registry& reg,
                                const glm::vec3& playerPos,
                                f32 maxDist)
{
    ecs::Entity best{};
    f32 bestD2 = maxDist * maxDist;

    auto& pool = reg.pool<NpcAI>();
    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* ai = pool.get(e);
        auto* tf = reg.get<Transform>(e);
        if (!ai || !tf) continue;
        if (ai->state == NpcAI::Dead) continue;

        glm::vec3 d = tf->position - playerPos;
        d.y = 0.f;
        f32 d2 = glm::dot(d, d);
        if (d2 < bestD2) {
            bestD2 = d2;
            best = e;
        }
    }
    return best;
}

} // namespace npc
