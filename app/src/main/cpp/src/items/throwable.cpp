/**
 * @file throwable.cpp
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#include "throwable.h"
#include "../ecs/components.h"
#include "../physics/raycast.h"
#include "../world/block.h"
#include "../combat/projectile.h"
#include "../combat/components.h"
#include "../audio/audio_events.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace items {

namespace {

/// Докуда летит бросок, метров.
constexpr f32 THROW_RANGE = 7.f;
/// Насколько глубоко ищем землю под точкой падения.
constexpr i32 GROUND_SEARCH = 16;

bool standable(u16 block) {
    if (block == world::AIR || block == world::UNKNOWN) return false;
    if (block == world::WATER || block == world::LAVA)  return false;
    return world::blocks().isSolid(block);
}

} // namespace

ecs::Entity throwTrampoline(ecs::Registry& reg,
                            world::ChunkManager& world,
                            const glm::vec3& origin,
                            const glm::vec3& dir)
{
    glm::vec3 land = origin + dir * THROW_RANGE;

    // Во что-то упёрлись — падаем чуть НЕ доходя: иначе точка падения
    // оказывается внутри стены, и батут ложится в камень.
    const physics::RayHit hit =
        physics::raycastVoxels(world, origin, dir, THROW_RANGE);
    if (hit.hit) land = origin + dir * std::max(0.f, hit.distance - 0.3f);

    const i32 bx = (i32)std::floor(land.x);
    const i32 bz = (i32)std::floor(land.z);
    const i32 from = (i32)std::floor(land.y);

    world::VoxelReader rd(world);
    i32 ground = -1;
    for (i32 y = from; y > 0 && y > from - GROUND_SEARCH; --y) {
        if (!standable(rd.at(bx, y - 1, bz))) continue;
        if (rd.at(bx, y, bz) != world::AIR) continue;   // занято
        ground = y;
        break;
    }
    if (ground < 0) return ecs::Entity{};

    const ecs::Entity e = reg.create();
    ecs::Transform tf;
    tf.position = { (f32)bx + 0.5f, (f32)ground, (f32)bz + 0.5f };
    reg.add(e, tf);
    reg.add(e, Trampoline{});
    reg.add(e, ecs::Kind{ ecs::EntityKind::Item });
    return e;
}

ecs::Entity throwShuriken(ecs::Registry& reg,
                          ecs::Entity thrower,
                          const glm::vec3& origin,
                          const glm::vec3& dir)
{
    combat::ProjectileSpawnParams p{};
    p.origin       = origin + dir * 0.5f;
    p.direction    = dir;
    p.ownerEntity  = (u32)thrower;
    p.ownerFaction = combat::Faction::of(reg, thrower);

    p.damage.amount       = SHURIKEN_DAMAGE;
    p.damage.type         = combat::DamageType::Physical;
    p.damage.sourceEntity = (u32)thrower;

    p.speed     = SHURIKEN_SPEED;
    p.gravity   = 0.f;     // настильно: дуга только мешала бы прицелу
    p.lifeTime  = 2.0f;    // и далеко не летит: это ближний бросок
    p.colorRGBA = 0xC8CED6FFu;
    p.scale     = 0.12f;
    p.isSpell   = false;

    // Бросок слышно. Отдельного звука у сюрикена нет, а выстрел из
    // лука — ровно то же событие: что-то маленькое ушло по воздуху.
    audio::events().arrowShoot(origin);
    return combat::spawnProjectile(reg, p);
}

void updateTrampolines(ecs::Registry& reg, f32 dt) {
    auto& pool = reg.pool<Trampoline>();
    std::vector<ecs::Entity> gone;

    for (usize i = 0; i < pool.size(); ++i) {
        const ecs::Entity e = pool.entityAt((u32)i);
        auto* t = pool.get(e);
        if (!t) continue;
        if (t->cooldown > 0.f) t->cooldown = std::max(0.f, t->cooldown - dt);
        t->lifeRemaining -= dt;
        if (t->lifeRemaining <= 0.f) gone.push_back(e);
    }
    for (ecs::Entity e : gone) reg.destroy(e);
}

f32 trampolineBounceAt(ecs::Registry& reg,
                       const glm::vec3& feet,
                       f32 verticalVelocity)
{
    if (verticalVelocity > 0.5f) return 0.f;

    auto& pool = reg.pool<Trampoline>();
    for (usize i = 0; i < pool.size(); ++i) {
        const ecs::Entity e = pool.entityAt((u32)i);
        auto* t  = pool.get(e);
        auto* tf = reg.get<ecs::Transform>(e);
        if (!t || !tf) continue;
        if (t->cooldown > 0.f) continue;

        const f32 dx = feet.x - tf->position.x;
        const f32 dz = feet.z - tf->position.z;
        if (std::fabs(dx) > t->radius || std::fabs(dz) > t->radius) continue;

        // По высоте окно намеренно широкое: игрок приходит на батут на
        // скорости, и за кадр ступни успевают уйти ниже площадки.
        const f32 dy = feet.y - tf->position.y;
        if (dy < -0.6f || dy > TRAMPOLINE_PAD_H + 0.5f) continue;

        t->cooldown = 0.3f;
        return t->bounce;
    }
    return 0.f;
}

} // namespace items
