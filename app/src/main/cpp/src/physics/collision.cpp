/**
 * @file collision.cpp
 * @brief Физика: AABB-коллизия с вокселями, raycast, контроллер персонажа.
 */
#include "collision.h"
#include "../world/block.h"
#include <cmath>
#include <algorithm>

namespace physics {

namespace {
constexpr f32 EPS = 1e-4f;
}

bool overlapsSolid(world::ChunkManager& world,
                   const glm::vec3& bmin,
                   const glm::vec3& bmax)
{
    i32 x0 = (i32)std::floor(bmin.x + EPS);
    i32 x1 = (i32)std::floor(bmax.x - EPS);
    i32 y0 = (i32)std::floor(bmin.y + EPS);
    i32 y1 = (i32)std::floor(bmax.y - EPS);
    i32 z0 = (i32)std::floor(bmin.z + EPS);
    i32 z1 = (i32)std::floor(bmax.z - EPS);

    // Курсор, а не getVoxel на каждый воксель: коробка почти всегда
    // целиком в одном чанке, а поиск чанка и два захвата замков стоят
    // в полсотни раз дороже самого чтения.
    auto& reg = world::blocks();
    world::VoxelReader rd(world);
    for (i32 y = y0; y <= y1; ++y)
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0; x <= x1; ++x)
                if (reg.isSolid(rd.at(x, y, z))) return true;
    return false;
}

// Движение по одной оси. Возвращает true, если упёрлись.
static bool moveAxis(world::ChunkManager& world,
                     glm::vec3& pos,
                     f32& delta,
                     const PlayerBox& box,
                     int axis,
                     CollisionFlags& flags)
{
    if (std::abs(delta) < 1e-7f) return false;

    pos[axis] += delta;

    glm::vec3 bmin = box.min(pos);
    glm::vec3 bmax = box.max(pos);

    // Диапазон вокселей, которые может задеть AABB
    i32 x0 = (i32)std::floor(bmin.x - EPS);
    i32 x1 = (i32)std::floor(bmax.x + EPS);
    i32 y0 = (i32)std::floor(bmin.y - EPS);
    i32 y1 = (i32)std::floor(bmax.y + EPS);
    i32 z0 = (i32)std::floor(bmin.z - EPS);
    i32 z1 = (i32)std::floor(bmax.z + EPS);

    bool found = false;
    i32 bestCoord = 0;
    auto& reg = world::blocks();
    world::VoxelReader rd(world);

    // Отсечение по ПУТИ, пройденному за кадр.
    //
    // Без него движение по вертикали разрешалось так: взять самый
    // высокий твёрдый блок, который задевает КОРПУС игрока, и
    // поставить ступни ему на верх. Блок сбоку на уровне ног — то
    // есть обычная ступенька, к которой игрок подошёл вплотную, —
    // попадал в этот перебор наравне с полом под ногами. И игрок
    // мгновенно оказывался на ней сверху: ровно то, что видно как
    // «телепортируется вверх». Диапазон перебора расширен на EPS, так
    // что хватало коснуться ступени краем коробки.
    //
    // Останавливать падение вправе только блок, чей ВЕРХ был не выше
    // ступней в начале кадра; подъём — только блок, чей НИЗ был не
    // ниже макушки. Всё остальное игрок задевает вбок, и вертикаль
    // это не касается.
    const f32 prevPos  = pos[axis] - delta;
    const f32 prevFeet = prevPos;
    const f32 prevHead = prevPos + box.height;

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 z = z0; z <= z1; ++z) {
            for (i32 x = x0; x <= x1; ++x) {
                if (!reg.isSolid(rd.at(x, y, z))) continue;
                if (axis == 1) {
                    if (delta < 0.f) {
                        // Верх блока обязан быть не выше прежних ступней.
                        if ((f32)(y + 1) > prevFeet + EPS) continue;
                    } else {
                        // Низ блока обязан быть не ниже прежней макушки.
                        if ((f32)y < prevHead - EPS) continue;
                    }
                }
                i32 c = (axis == 0) ? x : (axis == 1) ? y : z;
                if (!found) { bestCoord = c; found = true; }
                else if (delta > 0) { if (c < bestCoord) bestCoord = c; }
                else                { if (c > bestCoord) bestCoord = c; }
            }
        }
    }

    if (!found) return false;

    if (delta > 0) {
        if (axis == 1) {
            pos.y = (f32)bestCoord - box.height - EPS;
            flags.onCeiling = true;
        } else {
            pos[axis] = (f32)bestCoord - box.halfWidth - EPS;
        }
    } else {
        if (axis == 1) {
            pos.y = (f32)(bestCoord + 1) + EPS;
            flags.onGround = true;
        } else {
            pos[axis] = (f32)(bestCoord + 1) + box.halfWidth + EPS;
        }
    }
    delta = 0;
    if (axis == 0) flags.hitX = true;
    if (axis == 1) flags.hitY = true;
    if (axis == 2) flags.hitZ = true;
    return true;
}

CollisionFlags resolveMovement(world::ChunkManager& world,
                               glm::vec3& pos,
                               glm::vec3& delta,
                               const PlayerBox& box)
{
    CollisionFlags flags;

    // Порядок: Y сначала (гравитация, посадка), затем X, затем Z.
    // Так onGround проставляется до горизонтального движения.
    moveAxis(world, pos, delta.y, box, 1, flags);
    moveAxis(world, pos, delta.x, box, 0, flags);
    moveAxis(world, pos, delta.z, box, 2, flags);

    return flags;
}

} // namespace physics
