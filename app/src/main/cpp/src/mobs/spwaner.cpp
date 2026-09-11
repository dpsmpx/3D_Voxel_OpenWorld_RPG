#include "spawner.h"
#include "mob_def.h"
#include "mob_ai.h"
#include "../ecs/components.h"
#include "../combat/components.h"
#include "../world/block.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace mobs {

using namespace ecs;

namespace {

std::mt19937& rng() {
    static std::mt19937 g(0x12345678);
    return g;
}

u32 urand() { return rng()(); }

bool findSpawnSpot(world::ChunkManager& world,
                   i32 cx, i32 cz,
                   i32& outX, i32& outY, i32& outZ)
{
    auto& reg = world::blocks();
    for (int tries = 0; tries < 12; ++tries) {
        i32 x = cx * world::CHUNK_SIZE + (i32)(urand() % world::CHUNK_SIZE);
        i32 z = cz * world::CHUNK_SIZE + (i32)(urand() % world::CHUNK_SIZE);

        for (i32 y = world::CHUNK_SIZE_Y - 2; y > 1; --y) {
            const u16 b   = world.getVoxel(x, y, z);
            const u16 bel = world.getVoxel(x, y - 1, z);
            const u16 abv = world.getVoxel(x, y + 1, z);
            if (reg.isSolid(bel) && b == world::AIR && abv == world::AIR) {
                if (bel == world::WATER || bel == world::LAVA) break;
                outX = x; outY = y; outZ = z;
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool Spawner::isNight(f32 timeSec) const {
    f32 t = std::fmod(timeSec, 600.f) / 600.f;
    return (t > 0.5f);
}

u16 Spawner::pickMobId(world::TerrainGenerator& gen,
                       i32 x, i32 z, bool night, u32 rngVal) const
{
    world::BiomeId biome = gen.biomeAt(x, z);
    f32 r = (f32)(rngVal & 0xFFFF) / 65536.f;

    switch (biome) {
        case world::Ocean:      return MOB_NONE;
        case world::Beach:      return (r < 0.5f) ? MOB_CHICKEN : MOB_SHEEP;
        case world::Plains:
            return night ? ((r < 0.6f) ? MOB_SKELETON : MOB_GOBLIN)
                         : ((r < 0.5f) ? MOB_SHEEP    : MOB_COW);
        case world::Forest:
            return night ? ((r < 0.4f) ? MOB_WOLF
                         : (r < 0.75f) ? MOB_SKELETON
                                       : MOB_GOBLIN)
                         : ((r < 0.6f) ? MOB_SHEEP : MOB_COW);
        case world::Taiga:
            return night ? ((r < 0.5f) ? MOB_WOLF : MOB_SKELETON)
                         : ((r < 0.7f) ? MOB_SHEEP : MOB_COW);
        case world::Desert:
            return night ? MOB_SKELETON : ((r < 0.6f) ? MOB_CHICKEN : MOB_NONE);
        case world::Savanna:
            return night ? MOB_GOBLIN : MOB_COW;
        case world::Tundra:
            return (r < 0.6f) ? MOB_SHEEP : MOB_WOLF;
        case world::Mountains:
            return night ? MOB_GOBLIN : MOB_NONE;
        case world::Swamp:
            return (r < 0.5f) ? MOB_SLIME : MOB_WOLF;
        case world::Volcanic:
            return MOB_SLIME;
        default:
            return MOB_NONE;
    }
}

ecs::Entity Spawner::spawnMob(world::ChunkManager& /*world*/,
                              ecs::Registry& reg,
                              u16 mobId,
                              const glm::vec3& pos)
{
    const MobDef& def = mobRegistry().get(mobId);
    if (def.maxHealth <= 0.f) return {};

    ecs::Entity e = reg.create();

    ecs::Transform tf;
    tf.position = pos;
    reg.add(e, tf);

    reg.add(e, ecs::Velocity{});
    reg.add(e, ecs::Health{ def.maxHealth, def.maxHealth, 0.f, 0.f });
    reg.add(e, ecs::Collider{
        glm::vec3(def.bodyRadius, def.bodyHeight * 0.5f, def.bodyRadius),
        false
    });
    reg.add(e, ecs::Kind{
        def.hostile ? ecs::EntityKind::Mob : ecs::EntityKind::NPC
    });
    if (def.hostile) reg.add(e, ecs::EnemyTag{});
    reg.add(e, ecs::AIAgent{});
    reg.add(e, MobTag{ mobId });

    MobAI ai;
    ai.homePos = pos;
    ai.moveParams.bodyHeight  = def.bodyHeight;
    ai.moveParams.allowFall   = true;
    ai.moveParams.allowWater  = def.canSwim;
    ai.moveParams.allowJump   = true;
    reg.add(e, ai);

    // --- Боевые компоненты (Phase 8) ---
    combat::Combatant cmb;
    cmb.faction = def.hostile ? combat::Faction::Hostile
                              : combat::Faction::Passive;
    cmb.radius  = def.bodyRadius;
    cmb.height  = def.bodyHeight;
    if (def.hostile) {
        cmb.resistance.physical = 0.05f;
    }
    reg.add(e, cmb);

    reg.add(e, combat::StatusEffects{});

    return e;
}

void Spawner::update(world::ChunkManager& world,
                     ecs::Registry& reg,
                     const glm::vec3& playerPos,
                     f32 timeSec,
                     f32 dt)
{
    mobCount_ = (u32)reg.pool<MobTag>().size();

    const i32 pcx = (i32)std::floor(playerPos.x / world::CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / world::CHUNK_SIZE);

    spawnTimer_ += dt;
    if (spawnTimer_ >= 0.5f && mobCount_ < MAX_MOBS_TOTAL) {
        spawnTimer_ = 0.f;
        const bool night = isNight(timeSec);

        for (int attempt = 0; attempt < 6; ++attempt) {
            const i32 dx = (i32)(urand() % 7) - 3;
            const i32 dz = (i32)(urand() % 7) - 3;
            const i32 cx = pcx + dx;
            const i32 cz = pcz + dz;

            world::ChunkCoord ck{ cx, cz };
            if (perChunk_[ck] >= MAX_MOBS_PER_CHUNK) continue;

            i32 sx = 0, sy = 0, sz = 0;
            if (!findSpawnSpot(world, cx, cz, sx, sy, sz)) continue;

            const f32 ddx = (f32)sx - playerPos.x;
            const f32 ddz = (f32)sz - playerPos.z;
            const f32 dd  = std::sqrt(ddx * ddx + ddz * ddz);
            if (dd < SPAWN_RADIUS_MIN || dd > SPAWN_RADIUS) continue;

            const u32 rv = urand();
            const u16 id = pickMobId(
                const_cast<world::TerrainGenerator&>(world.generator()),
                sx, sz, night, rv);
            if (id == MOB_NONE) continue;

            const glm::vec3 pos { (f32)sx + 0.5f, (f32)sy, (f32)sz + 0.5f };
            if (spawnMob(world, reg, id, pos).valid()) {
                perChunk_[ck]++;
                ++mobCount_;
                break;
            }
        }
    }

    despawnTimer_ += dt;
    if (despawnTimer_ >= 2.0f) {
        despawnTimer_ = 0.f;

        std::vector<ecs::Entity> toRemove;
        auto& pool = reg.pool<MobAI>();
        for (usize i = 0; i < pool.size(); ++i) {
            const ecs::Entity e = pool.entityAt((u32)i);
            auto* tf = reg.get<ecs::Transform>(e);
            if (!tf) continue;

            glm::vec3 d = tf->position - playerPos;
            d.y = 0.f;
            if (glm::length(d) > DESPAWN_RADIUS) {
                toRemove.push_back(e);
            }
        }
        for (auto e : toRemove) reg.destroy(e);

        std::erase_if(perChunk_, [&](auto& kv) {
            const i32 dx = kv.first.x - pcx;
            const i32 dz = kv.first.z - pcz;
            return (dx * dx + dz * dz) > 100;
        });
    }
}

} // namespace mobs