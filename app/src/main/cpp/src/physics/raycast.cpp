#include "raycast.h"
#include "../world/block.h"
#include <cmath>

namespace physics {

RayHit raycastVoxels(world::ChunkManager& world,
                     const glm::vec3& origin,
                     const glm::vec3& dir,
                     f32 maxDist)
{
    RayHit hit;

    const f32 dirLen = glm::length(dir);
    if (dirLen < 1e-6f) return hit;
    const glm::vec3 d = dir / dirLen;

    i32 x = (i32)std::floor(origin.x);
    i32 y = (i32)std::floor(origin.y);
    i32 z = (i32)std::floor(origin.z);

    const i32 stepX = (d.x > 0.f) - (d.x < 0.f);
    const i32 stepY = (d.y > 0.f) - (d.y < 0.f);
    const i32 stepZ = (d.z > 0.f) - (d.z < 0.f);

    constexpr f32 INF = 1e30f;
    f32 tMaxX = (stepX != 0)
        ? ((f32)(x + (stepX > 0 ? 1 : 0)) - origin.x) / d.x
        : INF;
    f32 tMaxY = (stepY != 0)
        ? ((f32)(y + (stepY > 0 ? 1 : 0)) - origin.y) / d.y
        : INF;
    f32 tMaxZ = (stepZ != 0)
        ? ((f32)(z + (stepZ > 0 ? 1 : 0)) - origin.z) / d.z
        : INF;

    const f32 tDeltaX = (stepX != 0) ? std::abs(1.f / d.x) : INF;
    const f32 tDeltaY = (stepY != 0) ? std::abs(1.f / d.y) : INF;
    const f32 tDeltaZ = (stepZ != 0) ? std::abs(1.f / d.z) : INF;

    glm::ivec3 normal{0, 0, 0};
    f32 t = 0.f;

    // Начальный воксель: если игрок внутри блока — вернуть его
    auto& reg = world::blocks();
    u16 b0 = world.getVoxel(x, y, z);
    if (reg.isSolid(b0)) {
        hit.hit = true;
        hit.block = { x, y, z };
        hit.distance = 0.f;
        hit.blockType = b0;
        return hit;
    }

    constexpr i32 MAX_STEPS = 512;
    for (i32 i = 0; i < MAX_STEPS; ++i) {
        if (tMaxX < tMaxY) {
            if (tMaxX < tMaxZ) {
                x += stepX; t = tMaxX; tMaxX += tDeltaX; normal = { -stepX, 0, 0 };
            } else {
                z += stepZ; t = tMaxZ; tMaxZ += tDeltaZ; normal = { 0, 0, -stepZ };
            }
        } else {
            if (tMaxY < tMaxZ) {
                y += stepY; t = tMaxY; tMaxY += tDeltaY; normal = { 0, -stepY, 0 };
            } else {
                z += stepZ; t = tMaxZ; tMaxZ += tDeltaZ; normal = { 0, 0, -stepZ };
            }
        }

        if (t > maxDist) break;

        u16 b = world.getVoxel(x, y, z);
        if (reg.isSolid(b)) {
            hit.hit = true;
            hit.block = { x, y, z };
            hit.normal = normal;
            hit.distance = t;
            hit.blockType = b;
            return hit;
        }
    }

    return hit;
}

} // namespace physics
