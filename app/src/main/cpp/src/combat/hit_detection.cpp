/**
 * @file hit_detection.cpp
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#include "hit_detection.h"
#include "components.h"
#include "../ecs/components.h"
#include "../physics/raycast.h"
#include "../world/block.h"
#include <cmath>
#include <algorithm>

namespace combat {

namespace {

glm::vec3 entityCenter(ecs::Registry& reg, ecs::Entity e) {
    auto* tf = reg.get<ecs::Transform>(e);
    if (!tf) return glm::vec3(0);
    auto* col = reg.get<ecs::Collider>(e);
    f32 h = col ? col->halfExtents.y * 2.f : 1.8f;
    return tf->position + glm::vec3(0.f, h * 0.5f, 0.f);
}

f32 entityRadius(ecs::Registry& reg, ecs::Entity e) {
    auto* col = reg.get<ecs::Collider>(e);
    if (col) {
        f32 x = col->halfExtents.x;
        f32 z = col->halfExtents.z;
        return (x + z) * 0.5f;
    }
    return 0.4f;
}

} // namespace

bool losClear(world::ChunkManager& world,
              const glm::vec3& from,
              const glm::vec3& to)
{
    glm::vec3 d = to - from;
    f32 dist = glm::length(d);
    if (dist < 0.001f) return true;
    d /= dist;
    auto hit = physics::raycastVoxels(world, from, d, dist * 0.90f);
    return !hit.hit;
}

void meleeConeHits(world::ChunkManager& world,
                   ecs::Registry& reg,
                   const SpatialHash* hash,
                   const glm::vec3& origin,
                   const glm::vec3& forward,
                   f32 reach,
                   f32 coneAngle,
                   u32 attackerFaction,
                   ecs::Entity excludeEntity,
                   std::vector<HitTarget>& out)
{
    out.clear();

    f32 fwLen = glm::length(forward);
    if (fwLen < 1e-5f) return;
    const glm::vec3 fw = forward / fwLen;

    const f32 halfCos = std::cos(coneAngle * 0.5f);

    std::vector<ecs::Entity> candidates;
    if (hash) {
        hash->queryRadiusXZ(origin, reach + 1.f, candidates);
    } else {
        auto& hPool = reg.pool<ecs::Health>();
        candidates.reserve(hPool.size());
        for (usize i = 0; i < hPool.size(); ++i) {
            candidates.push_back(hPool.entityAt((u32)i));
        }
    }

    for (ecs::Entity e : candidates) {
        if (e == excludeEntity) continue;
        if (!reg.get<ecs::Transform>(e)) continue;
        if (!reg.get<ecs::Health>(e)) continue;

        const u32 f = Faction::of(reg, e);
        if (!Faction::hostileTo(attackerFaction, f)) continue;

        const glm::vec3 center = entityCenter(reg, e);
        const f32 radius = entityRadius(reg, e);

        glm::vec3 toT = center - origin;
        const f32 dist = glm::length(toT);
        if (dist > reach + radius) continue;
        if (dist < 1e-4f) {
            HitTarget h;
            h.entity = e;
            h.center = center;
            h.distance = 0.f;
            out.push_back(h);
            continue;
        }

        const glm::vec3 dir = toT / dist;
        const f32 dot = glm::dot(dir, fw);
        if (dot < halfCos) continue;

        if (!losClear(world, origin, center)) continue;

        HitTarget h;
        h.entity = e;
        h.center = center;
        h.distance = dist;
        out.push_back(h);
    }

    std::sort(out.begin(), out.end(),
              [](const HitTarget& a, const HitTarget& b) {
                  return a.distance < b.distance;
              });
}

void sphereHits(world::ChunkManager& world,
                ecs::Registry& reg,
                const SpatialHash* hash,
                const glm::vec3& center,
                f32 radius,
                u32 attackerFaction,
                ecs::Entity excludeEntity,
                std::vector<HitTarget>& out)
{
    out.clear();

    std::vector<ecs::Entity> candidates;
    if (hash) {
        hash->queryRadiusXZ(center, radius + 1.f, candidates);
    } else {
        auto& hPool = reg.pool<ecs::Health>();
        candidates.reserve(hPool.size());
        for (usize i = 0; i < hPool.size(); ++i) {
            candidates.push_back(hPool.entityAt((u32)i));
        }
    }

    for (ecs::Entity e : candidates) {
        if (e == excludeEntity) continue;
        if (!reg.get<ecs::Transform>(e)) continue;
        if (!reg.get<ecs::Health>(e)) continue;

        const u32 f = Faction::of(reg, e);
        if (!Faction::hostileTo(attackerFaction, f)) continue;

        const glm::vec3 c = entityCenter(reg, e);
        const f32 entityR = entityRadius(reg, e);

        const glm::vec3 d = c - center;
        const f32 dist = glm::length(d);
        if (dist > radius + entityR) continue;

        if (!losClear(world, center, c)) continue;

        HitTarget h;
        h.entity = e;
        h.center = c;
        h.distance = dist;
        out.push_back(h);
    }

    std::sort(out.begin(), out.end(),
              [](const HitTarget& a, const HitTarget& b) {
                  return a.distance < b.distance;
              });
}

void queryNearby(ecs::Registry& reg,
                 const glm::vec3& center,
                 f32 maxDistance,
                 std::vector<HitTarget>& out)
{
    out.clear();

    auto& hPool = reg.pool<ecs::Health>();
    for (usize i = 0; i < hPool.size(); ++i) {
        ecs::Entity e = hPool.entityAt((u32)i);
        if (!reg.get<ecs::Transform>(e)) continue;

        const glm::vec3 c = entityCenter(reg, e);
        const f32 dist = glm::length(c - center);
        if (dist > maxDistance) continue;

        HitTarget h;
        h.entity = e;
        h.center = c;
        h.distance = dist;
        out.push_back(h);
    }

    std::sort(out.begin(), out.end(),
              [](const HitTarget& a, const HitTarget& b) {
                  return a.distance < b.distance;
              });
}

} // namespace combat
