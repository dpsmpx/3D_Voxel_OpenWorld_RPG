/**
 * @file hazards.cpp
 * @brief Мир: ловушки в подземельях и замках.
 */
#include "hazards.h"
#include "features.h"
#include "terrain.h"
#include "../ecs/components.h"
#include "../combat/status_effects.h"
#include "../audio/audio_events.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace hazards {

namespace {

constexpr i32 SUPER_BLOCKS = world::SUPER_CHUNK_BLOCKS;

/// Ключ ловушки: ячейка сетки и номер внутри неё. Один на всю
/// программу — спавнер по нему решает, не заведена ли она уже.
inline u64 trapKey(i32 sx, i32 sz, u32 idx) {
    return ((u64)(u32)sx << 40) ^ ((u64)(u32)sz << 16) ^ (u64)idx;
}

} // namespace

void trapPointsNear(const glm::vec3& around, u64 worldSeed,
                    const world::TerrainGenerator& terrain,
                    std::vector<TrapPoint>& out)
{
    out.clear();
    const i32 sc0x = (i32)std::floor(around.x / (f32)SUPER_BLOCKS);
    const i32 sc0z = (i32)std::floor(around.z / (f32)SUPER_BLOCKS);

    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 sx = sc0x + dx, sz = sc0z + dz;

            // ---- Подземелье: ловушки в коридоре ----
            //
            // Коридор идёт из середины в ту же сторону, что и у
            // stampDungeon: угол считается из того же seed. Второй
            // копии правила здесь нет — есть тот же расчёт от того
            // же числа, и разойтись им нечем.
            const world::DungeonSite d = world::dungeonAt(sx, sz, worldSeed);
            if (d.exists) {
                const f32 angle = (f32)(d.seed & 0xFFFF) / 65535.f * 6.28318f;
                const f32 ax = std::cos(angle), az = std::sin(angle);
                // Через каждые десять шагов stampDungeon вырезает
                // комнату — ловушку ставим на подходе к ней, в
                // коридоре, а не в самой комнате: в комнате её
                // видно, в коридоре — нет.
                for (u32 i = 1; i <= 3; ++i) {
                    const i32 step = (i32)(i * 10) - 3;
                    TrapPoint p;
                    p.pos = { d.center.x + (i32)(ax * (f32)step),
                              d.center.y,
                              d.center.z + (i32)(az * (f32)step) };
                    p.key = trapKey(sx, sz, i);
                    out.push_back(p);
                }
            }

            // ---- Замок: ловушки во дворе, на пути к донжону ----
            const world::CastleSite c = world::castleAt(sx, sz, worldSeed, &terrain);
            if (c.exists) {
                const i32 offs[4][2] = {
                    { 0, 22 }, { -6, 16 }, { 6, 16 }, { 0, 12 },
                };
                for (u32 i = 0; i < 4; ++i) {
                    TrapPoint p;
                    p.pos = { c.center.x + offs[i][0],
                              c.center.y,
                              c.center.z + offs[i][1] };
                    p.key = trapKey(sx, sz, 100 + i);
                    out.push_back(p);
                }
            }
        }
}

void TrapSpawner::update(world::ChunkManager& world, ecs::Registry& reg,
                         const glm::vec3& playerPos, u64 worldSeed, f32 dt)
{
    // ---- Перезарядка и уборка ----
    {
        std::vector<ecs::Entity> gone;
        auto& pool = reg.pool<Trap>();
        for (usize i = 0; i < pool.size(); ++i) {
            const ecs::Entity e = pool.entityAt((u32)i);
            auto* t  = pool.get(e);
            auto* tf = reg.get<ecs::Transform>(e);
            if (!t || !tf) continue;
            if (!t->armed) {
                t->rearm -= dt;
                if (t->rearm <= 0.f) t->armed = true;
            }
            glm::vec3 d = tf->position - playerPos;
            d.y = 0.f;
            if (glm::length(d) > DESPAWN_DIST) gone.push_back(e);
        }
        for (auto e : gone) {
            reg.destroy(e);
            if (activeCount_ > 0) --activeCount_;
        }
    }

    // Раз в секунду: ловушки не переезжают.
    timer_ += dt;
    if (timer_ < 1.0f) return;
    timer_ = 0.f;

    std::vector<TrapPoint> points;
    trapPointsNear(playerPos, worldSeed, world.generator(), points);

    for (const auto& p : points) {
        const f32 ddx = (f32)p.pos.x - playerPos.x;
        const f32 ddz = (f32)p.pos.z - playerPos.z;
        if (ddx * ddx + ddz * ddz > SPAWN_DIST * SPAWN_DIST) continue;

        // Уже заведена?
        bool exists = false;
        {
            auto& pool = reg.pool<TrapTag>();
            for (usize i = 0; i < pool.size() && !exists; ++i) {
                auto* tag = pool.get(pool.entityAt((u32)i));
                if (tag && tag->key == p.key) exists = true;
            }
        }
        if (exists) continue;

        // Пол под ловушкой должен быть твёрдым: ловушка, висящая в
        // воздухе, не ловушка.
        if (!world.findChunk(p.pos.x >> 5, p.pos.z >> 5)) continue;
        if (world.getVoxel(p.pos.x, p.pos.y - 1, p.pos.z) == world::AIR) continue;

        const ecs::Entity e = reg.create();
        ecs::Transform tf;
        tf.position = { (f32)p.pos.x + 0.5f, (f32)p.pos.y, (f32)p.pos.z + 0.5f };
        reg.add(e, tf);
        reg.add(e, Trap{});
        reg.add(e, TrapTag{ p.key });
        ++activeCount_;
    }
}

f32 tickTraps(ecs::Registry& reg, ecs::Entity victim,
              const glm::vec3& victimPos, f32 dt)
{
    (void)dt;
    if (!victim.valid()) return 0.f;
    f32 dealt = 0.f;

    auto& pool = reg.pool<Trap>();
    for (usize i = 0; i < pool.size(); ++i) {
        const ecs::Entity e = pool.entityAt((u32)i);
        auto* t  = pool.get(e);
        auto* tf = reg.get<ecs::Transform>(e);
        if (!t || !tf || !t->armed) continue;

        glm::vec3 d = tf->position - victimPos;
        // По высоте — полтора блока: ловушка бьёт того, кто на неё
        // наступил, а не того, кто прошёл этажом выше.
        if (std::fabs(d.y) > 1.5f) continue;
        d.y = 0.f;
        if (glm::length(d) > t->radius) continue;

        combat::DamageInstance dmg;
        dmg.amount       = t->damage;
        dmg.type         = combat::DamageType::Physical;
        dmg.targetEntity = (u32)victim;
        dmg.sourceName   = "trap";
        dealt += combat::applyDamage(reg, victim, dmg);

        // Взводится обратно, а не исчезает: вернувшись той же
        // дорогой, игрок должен вспомнить, где наступил.
        t->armed = false;
        t->rearm = 6.f;
        audio::events().playerHurt();
    }
    return dealt;
}

} // namespace hazards
