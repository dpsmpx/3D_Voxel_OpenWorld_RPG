#include "occlusion.h"
#include "../world/block.h"
#include <algorithm>
#include <cmath>

namespace render {

namespace {

/// Чанк считается глухим, если на уровне глаз игрока по его
/// периметру нет воздуха. Проверяем срез по высоте, а не весь
/// объём: полный обход 131072 вокселей стоил бы дороже отрисовки.
constexpr i32 PROBE_Y_MIN = 8;
constexpr i32 PROBE_Y_MAX = 96;
constexpr i32 PROBE_STEP  = 8;

} // namespace

void OcclusionCuller::rebuild(world::ChunkManager& world,
                              const glm::vec3& cameraPos,
                              i32 radiusChunks)
{
    const i32 pcx = (i32)std::floor(cameraPos.x / (f32)world::CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(cameraPos.z / (f32)world::CHUNK_SIZE);

    side_   = radiusChunks * 2 + 1;
    origin_ = { pcx - radiusChunks, pcz - radiusChunks };
    opaque_.assign((usize)side_ * side_, 0);

    for (i32 dz = 0; dz < side_; ++dz) {
        for (i32 dx = 0; dx < side_; ++dx) {
            const i32 cx = origin_.x + dx;
            const i32 cz = origin_.z + dz;

            auto chunk = world.findChunk(cx, cz);
            if (!chunk) continue;                       // не загружен — не перекрывает
            if (!chunk->generated.load(std::memory_order_acquire)) continue;

            bool solid = true;
            {
                std::shared_lock lk(chunk->voxelMutex);
                for (i32 y = PROBE_Y_MIN; y <= PROBE_Y_MAX && solid; y += PROBE_STEP) {
                    for (i32 x = 0; x < world::CHUNK_SIZE && solid; x += 4) {
                        for (i32 z = 0; z < world::CHUNK_SIZE; z += 4) {
                            const u16 b = chunk->at(x, y, z);
                            if (b == world::AIR || world::blocks().isTransparent(b)) {
                                solid = false;
                                break;
                            }
                        }
                    }
                }
            }
            if (solid) opaque_[(usize)dz * side_ + dx] = 1;
        }
    }
}

bool OcclusionCuller::isOpaque(world::ChunkCoord c) const {
    const i32 dx = c.x - origin_.x;
    const i32 dz = c.z - origin_.z;
    if (dx < 0 || dz < 0 || dx >= side_ || dz >= side_) return false;
    return opaque_[(usize)dz * side_ + dx] != 0;
}

bool OcclusionCuller::isOccluded(world::ChunkCoord coord,
                                 const glm::vec3& cameraPos) const
{
    if (opaque_.empty()) return false;

    // Сам чанк глухой — рисовать в нём нечего.
    if (isOpaque(coord)) return true;

    const i32 ccx = (i32)std::floor(cameraPos.x / (f32)world::CHUNK_SIZE);
    const i32 ccz = (i32)std::floor(cameraPos.z / (f32)world::CHUNK_SIZE);
    if (ccx == coord.x && ccz == coord.z) return false;

    // Луч по сетке чанков (DDA в 2D). Если между камерой и целью
    // встретился глухой чанк, цель за ним не видна.
    i32 x = ccx, z = ccz;
    const i32 dx = coord.x - ccx;
    const i32 dz = coord.z - ccz;
    const i32 sx = dx > 0 ? 1 : -1;
    const i32 sz = dz > 0 ? 1 : -1;
    i32 ax = std::abs(dx), az = std::abs(dz);

    // Соседние чанки никогда не отсекаем: слишком близко, чтобы
    // между ними что-то поместилось.
    if (ax <= 1 && az <= 1) return false;

    i32 err = ax - az;
    ax *= 2; az *= 2;

    for (;;) {
        if (x == coord.x && z == coord.z) return false;
        if (err > 0)      { x += sx; err -= az; }
        else if (err < 0) { z += sz; err += ax; }
        else              { x += sx; z += sz; err += ax - az; }

        if (x == coord.x && z == coord.z) return false;
        if (isOpaque({ x, z })) return true;
    }
}

} // namespace render
