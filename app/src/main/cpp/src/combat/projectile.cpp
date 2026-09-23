/**
 * @file projectile.cpp
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#include "projectile.h"
#include "components.h"
#include "status_effects.h"
#include "../ecs/components.h"
#include "../physics/raycast.h"
#include "../world/block.h"
#include "../audio/audio_events.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace combat {

ecs::Entity spawnProjectile(ecs::Registry& reg,
                            const ProjectileSpawnParams& p)
{
    const f32 dirLenSq = glm::dot(p.direction, p.direction);
    if (dirLenSq < 1e-8f || !std::isfinite(dirLenSq) || !std::isfinite(p.speed)) return {};
    ecs::Entity e = reg.create();

    ecs::Transform tf;
    tf.position = p.origin;
    reg.add(e, tf);

    ecs::Velocity vel;
    vel.linear = p.direction * (p.speed / std::sqrt(dirLenSq));
    reg.add(e, vel);

    Projectile proj{};
    proj.weaponId          = p.weaponId;
    proj.velocity          = vel.linear;
    proj.lifeTime          = p.lifeTime;
    proj.lifeRemaining     = p.lifeTime;
    proj.ownerEntity       = p.ownerEntity;
    proj.ownerFaction      = p.ownerFaction;
    proj.damage            = p.damage;
    proj.secondary         = p.secondary;
    proj.lifesteal         = p.lifesteal;
    proj.affectedByGravity = (p.gravity > 0.f);
    proj.colorRGBA         = p.colorRGBA;
    proj.scale             = p.scale;
    proj.isSpell           = p.isSpell;
    reg.add(e, proj);

    reg.add(e, ecs::Kind{ ecs::EntityKind::Projectile });

    ecs::Collider col;
    col.halfExtents = glm::vec3(p.scale);
    reg.add(e, col);

    return e;
}

ecs::Entity spawnHitFx(ecs::Registry& reg,
                       const glm::vec3& position,
                       u32 colorRGBA,
                       f32 startScale,
                       f32 endScale,
                       f32 lifetime)
{
    ecs::Entity e = reg.create();

    ecs::Transform tf;
    tf.position = position;
    reg.add(e, tf);

    HitFx fx;
    fx.lifeTime      = lifetime;
    fx.lifeRemaining = lifetime;
    fx.startScale    = startScale;
    fx.endScale      = endScale;
    fx.colorRGBA     = colorRGBA;
    reg.add(e, fx);

    reg.add(e, ecs::Kind{ ecs::EntityKind::Effect });
    return e;
}

namespace {

ecs::Entity checkSegmentCandidate(ecs::Registry& reg,
                                  const glm::vec3& from,
                                  const glm::vec3& to,
                                  f32 projRadius,
                                  u32 ownerFaction,
                                  ecs::Entity ownerEntity,
                                  f32& outT)
{
    std::vector<ecs::Entity> candidates;

    auto& hp = reg.pool<ecs::Health>();
    for (usize i = 0; i < hp.size(); ++i) {
        ecs::Entity e = hp.entityAt((u32)i);
        if (e == ownerEntity) continue;
        if (!reg.get<ecs::Transform>(e)) continue;
        candidates.push_back(e);
    }

    const glm::vec3 segment = to - from;
    const f32 segLenSq = glm::dot(segment, segment);
    f32 bestT = 2.f;
    ecs::Entity best{};

    for (ecs::Entity e : candidates) {
        const u32 f = Faction::of(reg, e);
        if (!Faction::hostileTo(ownerFaction, f)) continue;

        auto* tf = reg.get<ecs::Transform>(e);
        if (!tf) continue;

        auto* col = reg.get<ecs::Collider>(e);
        f32 eh = col ? col->halfExtents.y * 2.f : 1.8f;
        f32 er = col ? (col->halfExtents.x + col->halfExtents.z) * 0.5f : 0.4f;

        glm::vec3 center = tf->position + glm::vec3(0.f, eh * 0.5f, 0.f);
        f32 t = 0.f;
        if (segLenSq > 1e-8f) {
            t = glm::dot(center - from, segment) / segLenSq;
            t = std::clamp(t, 0.f, 1.f);
        }
        const glm::vec3 closest = from + segment * t;
        const glm::vec3 d = center - closest;
        const f32 radius = er + projRadius;
        if (glm::dot(d, d) > radius * radius) continue;
        if (t < bestT) { bestT = t; best = e; }
    }
    outT = best.valid() ? bestT : 0.f;
    return best;
}

/// Звук попадания снаряда.
///
/// Стрелы и заклинания попадали молча: audio::AudioEvents::arrowHit и
/// spellHit были написаны и ни разу не позваны. Отличаем одно от
/// другого по тому же полю isSpell, по которому снаряд рисуется.
void projectileImpactSound(const Projectile& proj, const glm::vec3& pos) {
    if (proj.isSpell) audio::events().spellHit(pos, (u8)proj.damage.type);
    else              audio::events().arrowHit(pos);
}

} // namespace

void updateProjectiles(world::ChunkManager& world,
                       ecs::Registry& reg,
                       f32 dt)
{
    std::vector<ecs::Entity> list;
    {
        auto& pool = reg.pool<Projectile>();
        list.reserve(pool.size());
        for (usize i = 0; i < pool.size(); ++i)
            list.push_back(pool.entityAt((u32)i));
    }

    std::vector<ecs::Entity> toDestroy;

    for (ecs::Entity e : list) {
        auto* proj = reg.get<Projectile>(e);
        auto* tf   = reg.get<ecs::Transform>(e);
        auto* vel  = reg.get<ecs::Velocity>(e);
        if (!proj || !tf || !vel) continue;

        if (proj->affectedByGravity) {
            proj->velocity.y -= 9.8f * dt;
            vel->linear = proj->velocity;
        }

        const glm::vec3 startPos = tf->position;
        const glm::vec3 step = proj->velocity * dt;
        const glm::vec3 endPos = startPos + step;
        const f32 stepLen = glm::length(step);
        f32 entityT = 0.f;
        ecs::Entity entityTarget = checkSegmentCandidate(
            reg, startPos, endPos, proj->scale,
            proj->ownerFaction, reg.fromId(proj->ownerEntity), entityT);

        if (stepLen > 1e-5f) {
            const glm::vec3 dir = step / stepLen;
            auto vhit = physics::raycastVoxels(world, startPos, dir, stepLen);
            if (vhit.hit && (!entityTarget.valid() || entityT * stepLen > vhit.distance)) {
                tf->position = startPos + dir * vhit.distance;
                spawnHitFx(reg, tf->position, proj->colorRGBA,
                           0.15f, 0.55f, 0.18f);
                projectileImpactSound(*proj, tf->position);
                toDestroy.push_back(e);
                continue;
            }
        }

        if (entityTarget.valid()) {
            tf->position = startPos + step * entityT;
            ecs::Entity target = entityTarget;

            const f32 dmgMain = applyDamage(reg, target, proj->damage);

            if (proj->secondary.amount > 0.f ||
                proj->secondary.burnTime > 0.f ||
                proj->secondary.poisonTime > 0.f ||
                proj->secondary.slowDuration > 0.f ||
                proj->secondary.stunDuration > 0.f)
            {
                applyDamage(reg, target, proj->secondary);
            }

            if (proj->lifesteal > 0.f && dmgMain > 0.f) {
                auto* ah = reg.get<ecs::Health>(proj->ownerEntity);
                if (ah) {
                    ah->current = std::min(ah->max,
                                           ah->current + dmgMain * proj->lifesteal);
                }
            }

            if (auto* res = reg.get<ResonanceState>(proj->ownerEntity)) {
                res->onHit(proj->damage.isCritical);
            }

            spawnHitFx(reg, tf->position, proj->colorRGBA,
                       0.25f, 1.10f, 0.22f);
            projectileImpactSound(*proj, tf->position);
            toDestroy.push_back(e);
            continue;
        }

        tf->position = endPos;
        proj->lifeRemaining -= dt;
        if (proj->lifeRemaining <= 0.f) {
            toDestroy.push_back(e);
        }
    }

    for (ecs::Entity e : toDestroy) {
        reg.destroy(e);
    }
}

void updateHitFx(ecs::Registry& reg, f32 dt) {
    auto& pool = reg.pool<HitFx>();
    std::vector<ecs::Entity> toRemove;

    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* fx = pool.get(e);
        if (!fx) continue;

        fx->lifeRemaining -= dt;
        if (fx->lifeRemaining <= 0.f) toRemove.push_back(e);
    }

    for (ecs::Entity e : toRemove) reg.destroy(e);
}

} // namespace combat