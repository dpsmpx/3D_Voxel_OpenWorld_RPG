/**
 * @file crafting_station.cpp
 * @brief Крафт: рецепты, станции, проверка требований.
 */
#include "crafting_station.h"
#include "../ecs/components.h"
#include "../world/chunk_manager.h"
#include "../core/log.h"
#include <algorithm>
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