/**
 * @file terrain.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "terrain.h"
#include "block.h"
#include "chunk.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace world {

TerrainGenerator::TerrainGenerator(u64 seed)
    : biome_(seed),
      heightBase_(seed ^ 0xA55A),
      cavesA_(seed ^ 0xC0DE),
      cavesB_(seed ^ 0xBEEF),
      seed_(seed)
{}


namespace {

constexpr i32 RIVER_STEP = 8;
constexpr i32 RIVER_SOURCE_MARGIN = 96;
constexpr i32 RIVER_SOURCE_CANDIDATES = 12;
constexpr u32 RIVER_SOURCE_CHANCE = 82;
constexpr u32 RIVER_CACHE_MAX = 512;
constexpr i32 WORLD_TOP_SAFE = 118;

constexpr i32 D8X[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
constexpr i32 D8Z[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
constexpr i32 D16X[16] = {
     1, 1, 2, 2,  1, 0,-1,-2,
    -2,-2,-1, 0,  1, 2, 2, 1
};
constexpr i32 D16Z[16] = {
     0, 1, 1, 0, -1,-2,-2,-2,
    -1, 0,  1, 2,  2, 2, 1,-1
};

struct RiverTarget {
    i32 x = 0;
    i32 z = 0;
    i32 y = 0;
    bool sea = false;
};

static u32 riverHash(i32 x, i32 z, u64 seed) {
    u64 h = (u64)(u32)x * 0x9E3779B97F4A7C15ULL;
    h ^= (u64)(u32)z * 0xC4CEB9FE1A85EC53ULL;
    h ^= seed;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (u32)h;
}

static u64 riverCellKey(i32 x, i32 z) {
    return ((u64)(u32)x << 32) | (u32)z;
}

static i32 floorDiv8(i32 v) {
    if (v >= 0) return v / RIVER_STEP;
    return -(((-v) + RIVER_STEP - 1) / RIVER_STEP);
}

static u64 riverPosKey(i32 x, i32 z) {
    return ((u64)(u32)floorDiv8(x) << 32) | (u32)floorDiv8(z);
}

static i32 nearestDirTo(i32 fromX, i32 fromZ, i32 toX, i32 toZ) {
    const f32 tx = (f32)(toX - fromX);
    const f32 tz = (f32)(toZ - fromZ);
    const f32 len = std::hypot(tx, tz);
    if (len < 1e-4f) return 0;

    f32 bestDot = -2.f;
    i32 bestDir = 0;
    for (i32 d = 0; d < 8; ++d) {
        const f32 dot = (D8X[d] * tx + D8Z[d] * tz) / len *
                        0.70710678f;
        if (dot > bestDot) {
            bestDot = dot;
            bestDir = d;
        }
    }
    return bestDir;
}

static bool riverSourceUsable(const TerrainGenerator& terrain,
                              i32 x, i32 z, i32& scoreOut)
{
    const auto col = terrain.column(x, z);
    if (col.surface < TerrainGenerator::SEA_LEVEL + 40) return false;
    if (col.surface > WORLD_TOP_SAFE) return false;
    if (col.climate.biome == Ocean ||
        col.climate.biome == Beach ||
        col.climate.biome == Swamp ||
        col.climate.biome == Volcanic)
        return false;

    const bool mountainLike =
        col.climate.biome == Mountains ||
        (col.climate.peaks > 0.34f && col.climate.uplift > 0.18f);
    if (!mountainLike) return false;

    scoreOut = col.surface * 8
             + (i32)(col.climate.peaks * 180.f)
             + (i32)(col.climate.uplift * 100.f)
             + (i32)(col.climate.erosion * 20.f);
    return true;
}

static RiverTarget findRiverTarget(const TerrainGenerator& terrain,
                                   i32 sx, i32 sz)
{
    RiverTarget best{};
    i64 bestDist2 = std::numeric_limits<i64>::max();
    i32 bestLowScore = std::numeric_limits<i32>::max();

    constexpr i32 DISTANCES[] = {
        256, 512, 768, 1024, 1536, 2048, 3072, 4096
    };

    for (i32 dist : DISTANCES) {
        for (i32 d = 0; d < 16; ++d) {
            const i32 x = sx + D16X[d] * dist / 2;
            const i32 z = sz + D16Z[d] * dist / 2;
            const auto col = terrain.column(x, z);
            if (col.surface <= TerrainGenerator::SEA_LEVEL + 3 ||
                col.climate.biome == Ocean) {
                const i64 dx = (i64)x - sx;
                const i64 dz = (i64)z - sz;
                const i64 d2 = dx * dx + dz * dz;
                if (d2 < bestDist2) {
                    bestDist2 = d2;
                    best = {x, z, col.surface, true};
                }
            }
        }
        if (best.sea) return best;
    }

    for (i32 dist : DISTANCES) {
        for (i32 d = 0; d < 16; ++d) {
            const i32 x = sx + D16X[d] * dist / 2;
            const i32 z = sz + D16Z[d] * dist / 2;
            const auto col = terrain.column(x, z);
            const i32 score = col.surface +
                              (i32)std::max(0.f, col.climate.continent) * 12;
            if (score < bestLowScore) {
                bestLowScore = score;
                best = {x, z, col.surface, false};
            }
        }
    }
    return best;
}

static bool tooRecentlyVisited(
    const std::unordered_set<u64>& visited,
    const std::vector<TerrainGenerator::RiverPoint>& points,
    i32 x, i32 z)
{
    const u64 key = riverPosKey(x, z);
    if (visited.find(key) == visited.end()) return false;

    const usize keep = std::min<usize>(points.size(), 8);
    for (usize i = 0; i < keep; ++i) {
        const auto& p = points[points.size() - 1 - i];
        const i32 dx = p.x - x, dz = p.z - z;
        if (dx * dx + dz * dz <= RIVER_STEP * RIVER_STEP * 9)
            return true;
    }
    return false;
}

static TerrainGenerator::RiverPath traceRiver(
    const TerrainGenerator& terrain,
    i32 sx, i32 sz, i32 startWaterY,
    const RiverTarget& target, u32 seedSalt,
    f32 widthStart, f32 widthEnd, i32 maxSteps,
    bool tributary, i32 stopX = 0, i32 stopZ = 0,
    i32 stopDistance = 0, i32 stopWaterY = 0)
{
    TerrainGenerator::RiverPath path;
    path.tributary = tributary;
    path.points.reserve((usize)maxSteps + 2);

    const auto startCol = terrain.column(sx, sz);
    i32 currentX = sx;
    i32 currentZ = sz;
    i32 currentY = startCol.surface;
    i32 currentWaterY = std::min(startWaterY, currentY - 1);

    path.points.push_back({
        currentX, currentZ, (i16)currentY, (i16)currentWaterY, widthStart
    });

    i32 previousDir = nearestDirTo(sx, sz, target.x, target.z);

    std::unordered_set<u64> visited;
    visited.reserve((usize)maxSteps * 2);
    visited.insert(riverPosKey(currentX, currentZ));

    for (i32 step = 0; step < maxSteps; ++step) {
        const f32 t = maxSteps > 1
            ? (f32)step / (f32)(maxSteps - 1)
            : 1.f;

        if (currentY <= TerrainGenerator::SEA_LEVEL + 2) {
            currentWaterY = std::min(currentWaterY, TerrainGenerator::SEA_LEVEL);
            path.points.push_back({
                currentX, currentZ, (i16)currentY, (i16)currentWaterY,
                widthStart + (widthEnd - widthStart) * t
            });
            break;
        }

        if (stopDistance > 0) {
            const i32 dx = currentX - stopX;
            const i32 dz = currentZ - stopZ;
            if (dx * dx + dz * dz <= stopDistance * stopDistance) {
                const i32 finalWater = std::min(currentWaterY, stopWaterY);
                path.points.push_back({
                    stopX, stopZ, (i16)target.y, (i16)finalWater,
                    widthStart + (widthEnd - widthStart) * t
                });
                break;
            }
        }

        struct Candidate {
            i32 x = 0, z = 0, y = 0, dir = 0;
            f32 score = -1e9f;
            f32 drop = 0.f;
        };

        Candidate best{};
        Candidate bestDownhill{};
        bool haveBest = false;
        bool haveDownhill = false;

        const f32 currentTargetDist =
            std::hypot((f32)(target.x - currentX),
                       (f32)(target.z - currentZ));

        for (i32 d = 0; d < 8; ++d) {
            const i32 nx = currentX + D8X[d] * RIVER_STEP;
            const i32 nz = currentZ + D8Z[d] * RIVER_STEP;
            if (tooRecentlyVisited(visited, path.points, nx, nz)) continue;

            const auto col = terrain.column(nx, nz);
            const f32 drop = (f32)currentY - (f32)col.surface;
            const f32 nextWaterY =
                (f32)std::min(currentWaterY, col.surface - 1);
            const f32 channelDepth =
                (f32)col.surface - nextWaterY;

            const f32 nextTargetDist =
                std::hypot((f32)(target.x - nx),
                           (f32)(target.z - nz));
            const f32 targetGain = currentTargetDist - nextTargetDist;

            const f32 forward =
                (f32)(D8X[d] * D8X[previousDir] +
                      D8Z[d] * D8Z[previousDir]);

            const f32 uphill = drop < 0.f ? -drop : 0.f;
            const f32 noise =
                (f32)(riverHash(nx, nz, (u64)seedSalt) & 0xFFFF) / 65535.f;
            const f32 bend = 1.f - 0.5f * (forward + 1.f);
            const f32 depthPenalty =
                channelDepth > 6.f ? (channelDepth - 6.f) * 1.8f : 0.f;

            f32 score = drop * 3.4f
                      + targetGain * 0.032f
                      + forward * 1.1f
                      + noise * 0.28f
                      - uphill * 4.8f
                      - bend * 0.55f
                      - depthPenalty;

            if (col.surface <= TerrainGenerator::SEA_LEVEL + 3)
                score += 18.f;

            Candidate cand{nx, nz, col.surface, d, score, drop};
            if (!haveBest || cand.score > best.score) {
                best = cand;
                haveBest = true;
            }
            if (cand.drop >= 0.f &&
                (!haveDownhill || cand.score > bestDownhill.score)) {
                bestDownhill = cand;
                haveDownhill = true;
            }
        }

        if (!haveBest) break;

        if (haveDownhill && best.drop < -1.5f &&
            bestDownhill.score > best.score - 1.25f)
            best = bestDownhill;

        const i32 nextX = best.x;
        const i32 nextZ = best.z;
        const i32 nextY = best.y;
        i32 nextWaterY = std::min(currentWaterY, nextY - 1);

        if (target.sea && nextY <= TerrainGenerator::SEA_LEVEL + 3)
            nextWaterY = std::min(nextWaterY, TerrainGenerator::SEA_LEVEL);

        const f32 n =
            (f32)(riverHash(nextX, nextZ,
                            (u64)seedSalt ^ 0xBADC0DEULL) & 0xFFFF) / 65535.f;
        const f32 width = std::clamp(
            widthStart + (widthEnd - widthStart) * t + (n - 0.5f) * 0.7f,
            widthStart * 0.8f, widthEnd * 1.12f);

        path.points.push_back({
            nextX, nextZ, (i16)nextY, (i16)nextWaterY, width
        });

        currentX = nextX;
        currentZ = nextZ;
        currentY = nextY;
        currentWaterY = nextWaterY;
        previousDir = best.dir;
        visited.insert(riverPosKey(currentX, currentZ));

        if (step > 96 && path.points.size() >= 48) {
            const auto& a = path.points[path.points.size() - 1];
            const auto& b = path.points[path.points.size() - 33];
            const i32 moved = std::abs(a.x - b.x) + std::abs(a.z - b.z);
            const i32 fallen = (i32)b.terrainY - (i32)a.terrainY;
            if (moved < 32 && fallen <= 0 && target.sea &&
                step > maxSteps / 2)
                break;
        }
    }

    return path;
}

static bool findTributarySource(const TerrainGenerator& terrain,
                                const TerrainGenerator::RiverPoint& junction,
                                i32 branchIndex, i32& outX, i32& outZ,
                                i32& outY)
{
    i32 bestScore = std::numeric_limits<i32>::min();
    bool found = false;

    constexpr i32 RADII[] = {128, 192, 256, 320};
    for (i32 radius : RADII) {
        const u32 jh = riverHash(junction.x + branchIndex * 71,
                                 junction.z - branchIndex * 97,
                                 0x71B1ULL + (u64)radius);
        const i32 jitter = (i32)(jh & 31u) - 16;

        for (i32 d = 0; d < 16; ++d) {
            const i32 x = junction.x + D16X[d] * radius / 2
                        + jitter * (D16Z[d] != 0 ? 1 : 0);
            const i32 z = junction.z + D16Z[d] * radius / 2
                        - jitter * (D16X[d] != 0 ? 1 : 0);
            const auto col = terrain.column(x, z);

            if (col.surface < junction.waterY + 10) continue;
            if (col.surface < TerrainGenerator::SEA_LEVEL + 20) continue;
            if (col.surface > WORLD_TOP_SAFE) continue;
            if (col.climate.biome == Ocean || col.climate.biome == Beach ||
                col.climate.biome == Swamp || col.climate.biome == Volcanic)
                continue;

            const i32 dx = x - junction.x, dz = z - junction.z;
            const i32 dist2 = dx * dx + dz * dz;
            if (dist2 < 96 * 96) continue;

            const i32 score = col.surface * 9
                            + (i32)(col.climate.peaks * 120.f)
                            + (i32)(col.climate.uplift * 80.f)
                            - (i32)std::sqrt((f32)dist2) * 2;

            if (score > bestScore) {
                bestScore = score;
                outX = x;
                outZ = z;
                outY = col.surface;
                found = true;
            }
        }
    }
    return found;
}

static TerrainGenerator::RiverNetwork buildRiverNetwork(
    const TerrainGenerator& terrain, i32 cellX, i32 cellZ)
{
    TerrainGenerator::RiverNetwork net{};

    const u32 cellHash =
        riverHash(cellX, cellZ, terrain.seed() ^ 0x52A7EULL);
    if ((cellHash & 0xFFu) >= RIVER_SOURCE_CHANCE)
        return net;

    const i32 baseX = cellX * TerrainGenerator::RIVER_CELL_SIZE;
    const i32 baseZ = cellZ * TerrainGenerator::RIVER_CELL_SIZE;
    const i32 span = TerrainGenerator::RIVER_CELL_SIZE -
                     RIVER_SOURCE_MARGIN * 2;

    i32 bestScore = std::numeric_limits<i32>::min();
    i32 sx = 0, sz = 0, sy = 0;

    for (i32 i = 0; i < RIVER_SOURCE_CANDIDATES; ++i) {
        const u32 h = riverHash(cellX * 17 + i * 13,
                                cellZ * 31 + i * 7,
                                terrain.seed() ^ 0xA1E4ULL);
        const i32 x = baseX + RIVER_SOURCE_MARGIN + (i32)((h >> 8) % span);
        const i32 z = baseZ + RIVER_SOURCE_MARGIN + (i32)((h >> 20) % span);

        i32 score = 0;
        if (!riverSourceUsable(terrain, x, z, score)) continue;
        if (score > bestScore) {
            bestScore = score;
            sx = x;
            sz = z;
            sy = terrain.surfaceHeight(x, z);
        }
    }

    if (bestScore == std::numeric_limits<i32>::min()) return net;

    net.source = {sx, sy, sz};

    const RiverTarget target = findRiverTarget(terrain, sx, sz);
    TerrainGenerator::RiverPath main = traceRiver(
        terrain, sx, sz, sy - 1, target,
        riverHash(cellX, cellZ, terrain.seed() ^ 0xC011DULL),
        1.7f, 7.0f, TerrainGenerator::RIVER_MAX_LENGTH / RIVER_STEP,
        false);

    if (main.points.size() < 24) return net;
    net.paths.push_back(std::move(main));

    const usize mainSize = net.paths.front().points.size();
    const usize junctionCount = mainSize >= 180 ? 4 : 3;

    for (usize bi = 0; bi < junctionCount; ++bi) {
        const usize idx = (usize)((bi + 1) * mainSize /
                                  (junctionCount + 1));
        if (idx < 24 || idx + 16 >= mainSize) continue;

        const TerrainGenerator::RiverPoint& junction = net.paths.front().points[idx];

        i32 bx = 0, bz = 0, by = 0;
        if (!findTributarySource(terrain, junction, (i32)bi, bx, bz, by))
            continue;

        const RiverTarget branchTarget{
            junction.x, junction.z, junction.terrainY, false
        };

        TerrainGenerator::RiverPath branch = traceRiver(
            terrain, bx, bz, by - 1, branchTarget,
            riverHash(bx ^ (i32)(bi * 37),
                      bz ^ (i32)(bi * 53),
                      terrain.seed() ^ 0x7B1BULL),
            1.15f, 3.3f, 192, true,
            junction.x, junction.z, 18, junction.waterY);

        if (branch.points.size() < 16) continue;

        const auto& end = branch.points.back();
        const i32 dx = end.x - junction.x, dz = end.z - junction.z;
        if (dx * dx + dz * dz > 24 * 24) continue;
        if (end.waterY > junction.waterY + 1) continue;

        net.paths.push_back(std::move(branch));
    }

    return net;
}

} // namespace


std::shared_ptr<const TerrainGenerator::RiverNetwork>
TerrainGenerator::riverNetworkAtCell(i32 cellX, i32 cellZ) const
{
    const u64 key = riverCellKey(cellX, cellZ);
    {
        std::lock_guard lk(riverCacheMtx_);
        auto it = riverCache_.find(key);
        if (it != riverCache_.end()) return it->second;
    }

    auto generated = std::make_shared<RiverNetwork>(
        buildRiverNetwork(*this, cellX, cellZ));

    {
        std::lock_guard lk(riverCacheMtx_);
        auto [it, inserted] = riverCache_.emplace(key, generated);
        if (!inserted) return it->second;
        if (riverCache_.size() > RIVER_CACHE_MAX)
            riverCache_.erase(riverCache_.begin());
    }
    return generated;
}

void TerrainGenerator::riverNetworksNear(
    i32 chunkX, i32 chunkZ,
    std::vector<std::shared_ptr<const TerrainGenerator::RiverNetwork>>& out) const
{
    out.clear();

    const i32 wx = chunkX * CHUNK_SIZE + CHUNK_SIZE / 2;
    const i32 wz = chunkZ * CHUNK_SIZE + CHUNK_SIZE / 2;
    const i32 cx = (i32)std::floor((f32)wx / (f32)RIVER_CELL_SIZE);
    const i32 cz = (i32)std::floor((f32)wz / (f32)RIVER_CELL_SIZE);

    constexpr i32 radius =
        (RIVER_MAX_LENGTH + RIVER_CELL_SIZE - 1) / RIVER_CELL_SIZE;

    out.reserve((usize)(radius * 2 + 1) * (usize)(radius * 2 + 1));
    for (i32 dz = -radius; dz <= radius; ++dz)
        for (i32 dx = -radius; dx <= radius; ++dx) {
            auto net = riverNetworkAtCell(cx + dx, cz + dz);
            if (net && !net->paths.empty())
                out.push_back(std::move(net));
        }
}

TerrainGenerator::Column TerrainGenerator::column(i32 x, i32 z) const {
    Column col;

    // Шумовые поля не зависят от высоты — считаем их один раз.
    col.climate = biome_.fields(x, z);

    f32 h = 32.f + col.climate.heightMod;

    // Локальная детализация (мелкий рельеф)
    h += heightBase_.fbm3D((f32)x * 0.015f, 0.f, (f32)z * 0.015f, 3) * 4.f;

    // Скалы в горах
    if (col.climate.peaks > 0.3f && col.climate.erosion < 0.5f) {
        const f32 ridge =
            1.f - std::fabs(heightBase_.sample3D((f32)x * 0.03f, 0.f, (f32)z * 0.03f));
        h += ridge * col.climate.peaks * 12.f;
    }

    // Мягкий потолок вместо жёсткого обрезания.
    //
    // Хребты доходят до полутора сотен, а мир высотой сто двадцать
    // восемь: обрезание по линейке делало из вершин столовые горы —
    // ровные площадки ровно по потолку. Здесь верх сжимается и к
    // потолку только стремится, поэтому вершины разной высоты.
    if (h > 100.f) h = 100.f + (h - 100.f) / (1.f + (h - 100.f) * 0.06f);

    if (h < 1.f)   h = 1.f;
    if (h > 124.f) h = 124.f;
    col.surface = (i32)h;

    // Биом уточняется уже по фактической высоте.
    biome_.classify(col.climate, col.surface);

    // Кратер: у вулкана срезана верхушка, и в ней стоит лава.
    // Именно по нему вулкан и отличают от горы — не цветом камня.
    if (col.climate.biome == Volcanic && col.climate.uplift > 0.80f) {
        const f32 k = (col.climate.uplift - 0.80f) / 0.20f;
        const i32 depth = (i32)(k * 15.f);
        if (depth >= 3) {
            const i32 rim = col.surface;
            col.surface -= depth;
            if (col.surface < 2) col.surface = 2;
            // Лава не вровень с краем: полный до краёв кратер
            // читается как лужа, а не как жерло.
            col.lavaTop = rim - 3;
            if (col.lavaTop <= col.surface) col.lavaTop = 0;
        }
    }
    return col;
}

i32 TerrainGenerator::surfaceHeight(i32 x, i32 z) const {
    return column(x, z).surface;
}

BiomeId TerrainGenerator::biomeAt(i32 x, i32 z) const {
    return column(x, z).climate.biome;
}

BiomeField::Sample TerrainGenerator::sampleClimate(i32 x, i32 z, i32 surfaceY) const {
    return biome_.sample(x, z, surfaceY);
}

TerrainGenerator::CaveDensity
TerrainGenerator::caveDensity(f32 x, f32 y, f32 z) const {
    // Две ridged-поверхности: туннель там, где обе близки к единице.
    // По вертикали шум сжат в 1.6 раза — ходы получаются пологими.
    constexpr f32 SCALE = 1.f / 48.f;
    const f32 sx = x * SCALE, sy = y * SCALE * 1.6f, sz = z * SCALE;

    CaveDensity d;
    const f32 a = cavesA_.sample3D(sx, sy, sz);
    const f32 b = cavesB_.sample3D(sx, sy, sz);
    d.tunnel = std::min(1.f - std::fabs(a), 1.f - std::fabs(b));
    d.hall   = cavesA_.fbm3D(x * 0.008f, y * 0.015f, z * 0.008f, 3);
    return d;
}

bool TerrainGenerator::isCave(i32 x, i32 y, i32 z) const {
    return isCaveAt(caveDensity((f32)x, (f32)y, (f32)z), y);
}

u16 TerrainGenerator::oreAt(i32 x, i32 y, i32 z) const {
    // Руды через хэш-сетки. Идея: решаем принадлежность (x,y,z) к жиле
    // через хэш-ячейку 4×4×4. Внутри ячейки — шум для формы.

    constexpr i32 CELL = 4;
    i32 cx = x >> 2, cy = y >> 2, cz = z >> 2;

    // Детерминированный хэш
    auto hash3 = [](i32 a, i32 b, i32 c, u64 s) -> u32 {
        u64 h = (u64)(u32)a * 0x9E3779B97F4A7C15ULL;
        h ^= (u64)(u32)b * 0xC4CEB9FE1A85EC53ULL;
        h ^= (u64)(u32)c * 0xFF51AFD7ED558CCDULL;
        h ^= s;
        h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
        return (u32)h;
    };

    // Проверяем 3x3x3 ячейки-соседа — жила может заходить в текущую
    for (i32 ddy = -1; ddy <= 1; ++ddy) {
        for (i32 ddz = -1; ddz <= 1; ++ddz) {
            for (i32 ddx = -1; ddx <= 1; ++ddx) {
                i32 vcx = cx + ddx, vcy = cy + ddy, vcz = cz + ddz;
                u32 h = hash3(vcx, vcy, vcz, seed_ ^ 0x0AE5ULL);
                // 12% ячеек содержат жилу
                if ((h & 0xFF) > 30) continue;
                // Тип руды зависит от высоты
                u16 oreKind;
                if (vcy * CELL > 80) continue;   // нет руды на поверхности
                if (vcy * CELL > 50) {
                    oreKind = ((h >> 8) & 3) == 0 ? GOLD_ORE : IRON_ORE;
                } else {
                    oreKind = ((h >> 8) & 3) < 2 ? IRON_ORE : GOLD_ORE;
                }
                // Центр жилы внутри ячейки
                f32 ox = (f32)(h & 0xF) / 16.f;
                f32 oy = (f32)((h >> 8) & 0xF) / 16.f;
                f32 oz = (f32)((h >> 16) & 0xF) / 16.f;
                glm::vec3 center = {
                    (f32)(vcx * CELL) + ox * (f32)CELL,
                    (f32)(vcy * CELL) + oy * (f32)CELL,
                    (f32)(vcz * CELL) + oz * (f32)CELL,
                };
                glm::vec3 p = { (f32)x + 0.5f, (f32)y + 0.5f, (f32)z + 0.5f };
                glm::vec3 d = p - center;
                f32 dist2 = glm::dot(d, d);
                // Радиус жилы 1.5..2.5
                f32 r = 1.5f + (f32)((h >> 24) & 0xFF) / 255.f;
                if (dist2 < r * r) return oreKind;
            }
        }
    }
    return STONE;
}

} // namespace world
