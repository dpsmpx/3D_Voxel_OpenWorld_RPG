#include "projectile.h"
#include "components.h"
#include "status_effects.h"
#include "../ecs/components.h"
#include "../physics/raycast.h"
#include "../world/block.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace combat {

ecs::Entity spawnProjectile(ecs::Registry& reg,
                            const ProjectileSpawnParams& p)
{
    ecs::Entity e = reg.create();

    ecs::Transform tf;
    tf.position = p.origin;
    reg.add(e, tf);

    ecs::Velocity vel;
    vel.linear = glm::normalize(p.direction) * p.speed;
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
    col.isStatic    = false;
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

ecs::Entity projectileCheckEntityHit(ecs::Registry& reg,
                                     const Projectile& /*proj*/,
                                     const glm::vec3& /*position*/)
{
    (void)reg;
    return {};
}

namespace {

ecs::Entity checkOneCandidate(ecs::Registry& reg,
                              const glm::vec3& pos,
                              f32 projRadius,
                              u32 ownerFaction,
                              ecs::Entity ownerEntity)
{
    std::vector<ecs::Entity> candidates;

    auto& hp = reg.pool<ecs::Health>();
    for (usize i = 0; i < hp.size(); ++i) {
        ecs::Entity e = hp.entityAt((u32)i);
        if (e == ownerEntity) continue;
        if (!reg.get<ecs::Transform>(e)) continue;
        candidates.push_back(e);
    }

    for (ecs::Entity e : candidates) {
        const u32 f = Faction::of(reg, e);
        if (!Faction::hostileTo(ownerFaction, f)) continue;

        auto* tf = reg.get<ecs::Transform>(e);
        if (!tf) continue;

        auto* col = reg.get<ecs::Collider>(e);
        f32 eh = col ? col->halfExtents.y * 2.f : 1.8f;
        f32 er = col ? (col->halfExtents.x + col->halfExtents.z) * 0.5f : 0.4f;

        glm::vec3 center = tf->position + glm::vec3(0.f, eh * 0.5f, 0.f);
        glm::vec3 d = center - pos;
        f32 dist = glm::length(d);
        if (dist > er + projRadius) continue;

        return e;
    }
    return {};
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

        const glm::vec3 step = proj->velocity * dt;
        const f32 stepLen = glm::length(step);

        if (stepLen > 1e-5f) {
            const glm::vec3 dir = step / stepLen;
            auto vhit = physics::raycastVoxels(world, tf->position, dir, stepLen);
            if (vhit.hit) {
                tf->position += dir * vhit.distance;
                spawnHitFx(reg, tf->position, proj->colorRGBA,
                           0.15f, 0.55f, 0.18f);
                toDestroy.push_back(e);
                continue;
            }
        }

        tf->position += step;

        ecs::Entity target = checkOneCandidate(
            reg, tf->position, proj->scale,
            proj->ownerFaction, proj->ownerEntity);

        if (target.valid()) {
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
            toDestroy.push_back(e);
            continue;
        }

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