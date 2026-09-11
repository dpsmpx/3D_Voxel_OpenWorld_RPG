#include "crafting_station.h"
#include "../ecs/components.h"
#include "../world/chunk_manager.h"
#include "../core/log.h"
#include <cmath>
#include <vector>

namespace crafting {

using namespace ecs;

namespace {

// Тот же хэш, что в features.cpp / npc_spawner.cpp.
u32 hashXZ(i32 x, i32 z, u64 seed) {
    u64 h = (u64)(u32)x * 0x9E3779B97F4A7C15ULL;
    h ^= (u64)(u32)z * 0xC4CEB9FE1A85EC53ULL;
    h ^= seed;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (u32)h;
}

// Возвращает true, если super-chunk — деревня.
bool isVillage(i32 sx, i32 sz, u64 worldSeed) {
    u32 h = hashXZ(sx, sz, worldSeed ^ 0x517);
    u32 sel = h & 0xFF;
    return sel < 51;   // см. features::structs::layoutFor
}

struct StationSpec {
    StationType type;
    glm::vec3   pos;
};

bool generateVillageStations(world::ChunkManager& world,
                             i32 sx, i32 sz, u64 worldSeed,
                             std::vector<StationSpec>& out)
{
    if (!isVillage(sx, sz, worldSeed)) return false;

    const u32 h = hashXZ(sx, sz, worldSeed ^ 0x517);
    const i32 baseX = sx * 256;
    const i32 baseZ = sz * 256;
    const i32 ox = (i32)((h >> 8) & 0x3F);
    const i32 oz = (i32)((h >> 14) & 0x3F);
    const i32 cx = baseX + ox + 32;
    const i32 cz = baseZ + oz + 32;

    out.clear();

    auto place = [&](StationType t, f32 dx, f32 dz) {
        StationSpec s;
        s.type = t;
        i32 px = cx + (i32)dx;
        i32 pz = cz + (i32)dz;
        i32 py = world.generator().surfaceHeight(px, pz);
        s.pos = { (f32)px + 0.5f, (f32)py + 0.2f, (f32)pz + 0.5f };
        out.push_back(s);
    };

    // Верстак — рядом с центром
    place(StationType::Workbench,  8.f,  8.f);

    // Наковальня — у кузнеца (который в (-5, -3))
    place(StationType::Anvil,     -7.f, -3.f);

    // Алхимический стол — у лекаря (который в (5, 5))
    place(StationType::Alchemy,    7.f,  5.f);

    // В больших деревнях — второй верстак
    if ((h >> 24) & 1) {
        place(StationType::Workbench, -8.f, 7.f);
    }
    return true;
}

} // namespace

void StationSpawner::update(ecs::Registry& reg,
                            world::ChunkManager& world,
                            const glm::vec3& playerPos,
                            u64 worldSeed)
{
    spawnTimer_ += 1.f / 60.f;
    despawnTimer_ += 1.f / 60.f;

    // ---- Спавн ----
    if (spawnTimer_ >= 0.75f) {
        spawnTimer_ = 0.f;

        const i32 playerSx = (i32)std::floor(playerPos.x / (f32)SUPER_BLOCKS);
        const i32 playerSz = (i32)std::floor(playerPos.z / (f32)SUPER_BLOCKS);

        for (i32 dz = -2; dz <= 2; ++dz) {
            for (i32 dx = -2; dx <= 2; ++dx) {
                const i32 sx = playerSx + dx;
                const i32 sz = playerSz + dz;

                // Уже заспавнены?
                bool anyExists = false;
                {
                    auto& pool = reg.pool<CraftingStation>();
                    for (usize i = 0; i < pool.size(); ++i) {
                        ecs::Entity e = pool.entityAt((u32)i);
                        auto* tf = reg.get<Transform>(e);
                        if (!tf) continue;
                        i32 nSx = (i32)std::floor(tf->position.x / (f32)SUPER_BLOCKS);
                        i32 nSz = (i32)std::floor(tf->position.z / (f32)SUPER_BLOCKS);
                        if (nSx == sx && nSz == sz) {
                            anyExists = true;
                            break;
                        }
                    }
                }
                if (anyExists) continue;

                std::vector<StationSpec> specs;
                if (!generateVillageStations(world, sx, sz, worldSeed, specs)) continue;

                // Не спавним далёкие
                f32 firstDX = specs.empty() ? 9999.f : specs[0].pos.x - playerPos.x;
                f32 firstDZ = specs.empty() ? 9999.f : specs[0].pos.z - playerPos.z;
                if (firstDX * firstDX + firstDZ * firstDZ > SPAWN_DIST * SPAWN_DIST)
                    continue;

                for (const auto& spec : specs) {
                    ecs::Entity e = reg.create();

                    Transform tf;
                    tf.position = spec.pos;
                    reg.add(e, tf);

                    CraftingStation st;
                    st.type = spec.type;
                    switch (st.type) {
                        case StationType::Workbench:
                            st.colorRGBA = 0xB08040FF;
                            break;
                        case StationType::Anvil:
                            st.colorRGBA = 0x404040FF;
                            break;
                        case StationType::Alchemy:
                            st.colorRGBA = 0x9040C0FF;
                            break;
                        default: break;
                    }
                    reg.add(e, st);

                    Collider col;
                    col.halfExtents = glm::vec3(st.sizeX * 0.5f,
                                                st.sizeY * 0.5f,
                                                st.sizeZ * 0.5f);
                    col.isStatic = true;
                    reg.add(e, col);

                    reg.add(e, Kind{ EntityKind::Structure });

                    ++activeCount_;
                }
            }
        }
    }

    // ---- Деспавн ----
    if (despawnTimer_ >= 2.0f) {
        despawnTimer_ = 0.f;

        std::vector<ecs::Entity> toRemove;
        auto& pool = reg.pool<CraftingStation>();
        for (usize i = 0; i < pool.size(); ++i) {
            ecs::Entity e = pool.entityAt((u32)i);
            auto* tf = reg.get<Transform>(e);
            if (!tf) continue;
            glm::vec3 d = tf->position - playerPos;
            d.y = 0.f;
            if (glm::length(d) > DESPAWN_DIST) toRemove.push_back(e);
        }
        for (auto e : toRemove) {
            reg.destroy(e);
            if (activeCount_ > 0) --activeCount_;
        }
    }
}

} // namespace crafting