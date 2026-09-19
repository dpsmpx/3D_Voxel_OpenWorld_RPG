/**
 * @file spawn.cpp
 * @brief Мир: выбор точки, в которой игрок появляется впервые.
 */
#include "spawn.h"
#include "features.h"
#include "block.h"
#include <memory>
#include <vector>
#include <cstdlib>

namespace world {

namespace {

/// Насколько соседние колонки могут расходиться по высоте.
///
/// Четыре блока на шаг в четыре — уклон в сорок пять градусов. Круче
/// — это уже не склон, а стена: игрок появляется прижатым к ней и
/// половину обзора упирает в камень.
constexpr i32 MAX_SLOPE = 4;

/// Живётся ли в биоме.
///
/// Океан — вода, вулкан — лава, Чёрный лес — твари, которые не спят
/// днём. Первые два убивают сразу, третий — через полминуты.
bool habitable(BiomeId b) {
    return b != Ocean && b != Volcanic && b != Blight;
}

/// Ячейка сетки структур, которой принадлежит координата.
///
/// Сдвиг, а не деление: у деления знак усечён к нулю, и к западу от
/// нуля блоки -1 и -256 попали бы в одну ячейку с блоком 0.
i32 superChunkOf(i32 w) {
    static_assert(SUPER_CHUNK_BLOCKS == 256, "сдвиг на 8 считает ячейку");
    return w >> 8;
}

} // namespace

const char* spawnRejectName(SpawnReject r) {
    switch (r) {
        case SpawnReject::Ok:         return "ok";
        case SpawnReject::UnderWater: return "under water";
        case SpawnReject::Steep:      return "steep";
        case SpawnReject::BadBiome:   return "bad biome";
        case SpawnReject::Cave:       return "cave below";
        case SpawnReject::Structure:  return "structure";
        case SpawnReject::Sinkhole:   return "sinkhole";
        case SpawnReject::Lair:       return "lair";
        case SpawnReject::Blocked:    return "blocked";
        default:                      return "?";
    }
}

SpawnReject spawnPointCheck(const TerrainGenerator& terrain, u64 worldSeed,
                            i32 wx, i32 wz, i32& outY)
{
    // Колонка: высота и климат за один запрос. Спрашивать их порознь
    // значит дважды посчитать один и тот же шум.
    const TerrainGenerator::Column col = terrain.column(wx, wz);
    const i32 surf = col.surface;
    outY = surf;

    // Проверки идут от дешёвых к дорогим: колонка уже на руках, и
    // почти все отказы разрешаются по ней одной. Перебор структур —
    // девять супер-чанков — достаётся единицам точек.

    // surfaceHeight — первый воздушный блок; вода наливается от него
    // и до уровня моря, поэтому суша начинается на блок выше.
    if (surf <= TerrainGenerator::SEA_LEVEL + 1) return SpawnReject::UnderWater;
    if (!habitable(col.climate.biome))           return SpawnReject::BadBiome;

    for (i32 i = 0; i < 4; ++i) {
        static const i32 dx[4] = { SPAWN_STEP, -SPAWN_STEP, 0, 0 };
        static const i32 dz[4] = { 0, 0, SPAWN_STEP, -SPAWN_STEP };
        const i32 h = terrain.surfaceHeight(wx + dx[i], wz + dz[i]);
        if (std::abs(h - surf) > MAX_SLOPE) return SpawnReject::Steep;
    }

    // Пещеры вырезаются ПОСЛЕ слоёв рельефа, и колонка о них не
    // знает: земля под ногами может оказаться сводом пустоты.
    if (terrain.isCave(wx, surf - 1, wz)) return SpawnReject::Cave;

    if (structureCovers(wx, wz, worldSeed, &terrain)) return SpawnReject::Structure;

    // Провал — дыра с отвесными стенами: появиться в ней значит
    // очнуться в падении. Соседние ячейки сетки тоже спрашиваем:
    // центр провала смещён внутри ячейки, и устье заходит к соседу.
    for (i32 dz2 = -1; dz2 <= 1; ++dz2)
        for (i32 dx2 = -1; dx2 <= 1; ++dx2) {
            const i32 sx = superChunkOf(wx) + dx2;
            const i32 sz = superChunkOf(wz) + dz2;
            const SinkholeSite s = sinkholeAt(sx, sz, worldSeed, &terrain);
            if (!s.exists) continue;
            const i32 ddx = wx - s.center.x, ddz = wz - s.center.z;
            const i32 r = s.radius + 2;       // край осыпается — отойти
            if (ddx * ddx + ddz * ddz <= r * r) return SpawnReject::Sinkhole;
        }

    // Логово блоков не ставит, но появиться в волчьей стае — это
    // появиться и сразу умереть.
    if (lairCovering(wx, wz, worldSeed, &terrain).exists) return SpawnReject::Lair;

    return SpawnReject::Ok;
}

const Chunk* SpawnVoxelProbe::chunkAt(i32 cx, i32 cz) {
    if (have_ && haveX_ == cx && haveZ_ == cz) return chunk_.get();

    if (!chunk_) chunk_ = std::make_unique<Chunk>();
    chunk_->coord = { cx, 0, cz };

    std::vector<TerrainGenerator::Column> cols;
    computeChunkColumns(*terrain_, cx, cz, cols);
    generateChunkVoxels(*chunk_, *terrain_, cols.data(), seed_);

    haveX_ = cx; haveZ_ = cz; have_ = true;
    ++built_;
    return chunk_.get();
}

bool SpawnVoxelProbe::standable(i32 wx, i32 wy, i32 wz) {
    if (wy < 1 || wy + 1 >= CHUNK_SIZE_Y) return false;

    const Chunk* c = chunkAt(wx >> 5, wz >> 5);
    if (!c) return false;
    const i32 lx = wx & 31, lz = wz & 31;

    const BlockRegistry& reg = blocks();
    // Ноги и голова — в пустоте. Листва твёрдая: игрок, очнувшийся
    // внутри кроны, без кирки из неё не выйдет.
    if (reg.isSolid(c->at(lx, wy,     lz))) return false;
    if (reg.isSolid(c->at(lx, wy + 1, lz))) return false;
    // А под ногами — твёрдое: иначе это не земля, а дыра.
    return reg.isSolid(c->at(lx, wy - 1, lz));
}

SpawnSearch findSpawn(const TerrainGenerator& terrain, u64 worldSeed,
                      i32 centerX, i32 centerZ, i32 halfExtent,
                      const SpawnVoxelTest& voxels)
{
    SpawnSearch out;
    if (halfExtent < 0) halfExtent = 0;
    const i32 maxRing = halfExtent / SPAWN_STEP;

    // Кольцами наружу. Кольцо r — это рамка ячеек, у которых
    // max(|i|,|j|) == r; внутренность рамки уже просмотрена на
    // предыдущих кольцах, и повторять её незачем.
    for (i32 r = 0; r <= maxRing; ++r) {
        for (i32 j = -r; j <= r; ++j)
            for (i32 i = -r; i <= r; ++i) {
                const i32 ai = (i < 0) ? -i : i;
                const i32 aj = (j < 0) ? -j : j;
                if (ai != r && aj != r) continue;

                const i32 wx = centerX + i * SPAWN_STEP;
                const i32 wz = centerZ + j * SPAWN_STEP;
                i32 y = 0;
                const SpawnReject why = spawnPointCheck(terrain, worldSeed, wx, wz, y);
                ++out.tried;

                // Воксели спрашиваются ПОСЛЕДНИМИ и только у тех
                // точек, что прошли всё остальное: один такой вопрос
                // стоит целого сгенерированного чанка.
                SpawnReject verdict = why;
                if (verdict == SpawnReject::Ok && voxels && !voxels(wx, y, wz))
                    verdict = SpawnReject::Blocked;

                out.rejected[(u32)verdict] += 1;
                if (verdict != SpawnReject::Ok) continue;

                out.found = true;
                out.point = { wx, y, wz };
                return out;
            }
    }
    return out;
}

glm::vec3 spawnPositionOrFallback(const TerrainGenerator& terrain,
                                  u64 worldSeed, i32 centerX, i32 centerZ,
                                  const SpawnVoxelTest& voxels)
{
    const SpawnSearch s = findSpawn(terrain, worldSeed, centerX, centerZ,
                                    SPAWN_HALF_EXTENT, voxels);

    // Середина блока по X и Z: игрок шириной в шестьдесят сантиметров,
    // поставленный на угол, наполовину в соседнем блоке.
    if (s.found)
        return { (f32)s.point.x + 0.5f, (f32)s.point.y, (f32)s.point.z + 0.5f };

    // Не нашлось ничего. На уровень моря в середине области: там
    // вода, а в воде не тонут — плывут.
    return { (f32)centerX + 0.5f,
             (f32)(TerrainGenerator::SEA_LEVEL + 1),
             (f32)centerZ + 0.5f };
}

} // namespace world
