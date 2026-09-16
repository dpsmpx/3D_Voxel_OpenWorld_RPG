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

    // Диапазон вокселей, которые перебираем.
    //
    // Вдоль ОСИ ДВИЖЕНИЯ рамка расширена на EPS: к грани блока,
    // в который упираемся, игрок приставлен вплотную, и без запаса
    // этот блок в перебор не попал бы.
    //
    // По ДВУМ ДРУГИМ осям — наоборот, сжата: там блок мешает, только
    // если коробка его ПЕРЕКРЫВАЕТ, а не просто касается гранью.
    // Расширение по ним и было второй половиной беды. Игрок стоит на
    // полу, ступни ровно на границе блока, — и слой пола под ногами
    // попадал в перебор горизонтального движения. Пол твёрдый в
    // каждом столбце, поэтому «препятствие впереди» находилось на
    // ровном месте, всегда: шаг вперёд упирался в собственный пол.
    //
    // Ровно так же это понимает overlapsSolid — сжатой рамкой. Две
    // функции про одно и то же расходились в том, что считать
    // пересечением, и расходились молча.
    const bool alongX = (axis == 0), alongY = (axis == 1), alongZ = (axis == 2);
    const f32 padX = alongX ? -EPS : EPS;
    const f32 padY = alongY ? -EPS : EPS;
    const f32 padZ = alongZ ? -EPS : EPS;

    i32 x0 = (i32)std::floor(bmin.x + padX);
    i32 x1 = (i32)std::floor(bmax.x - padX);
    i32 y0 = (i32)std::floor(bmin.y + padY);
    i32 y1 = (i32)std::floor(bmax.y - padY);
    i32 z0 = (i32)std::floor(bmin.z + padZ);
    i32 z1 = (i32)std::floor(bmax.z - padZ);

    bool found = false;
    i32 bestCoord = 0;
    auto& reg = world::blocks();
    world::VoxelReader rd(world);

    // Отсечение по ПУТИ, пройденному за кадр. Правило одно на все три
    // оси — и это главное, что здесь было не так.
    //
    // Раньше препятствием считался КРАЙНИЙ твёрдый блок из всех, что
    // задевает коробка игрока, без единой проверки, что он вообще
    // впереди по ходу движения. Коробка шириной 0.6 всегда задевает
    // два столбца, и любой блок В СВОЁМ ЖЕ столбце — косяк двери,
    // козырёк на высоте головы, угол стены рядом — выигрывал этот
    // перебор. Игрока приставляли к его ДАЛЬНЕЙ грани, то есть
    // отбрасывали НАЗАД почти на целый блок за кадр, хотя он шёл
    // вперёд. Замер на стенде: шаг −1.000 по X и −1.000 по Z в одном
    // кадре, и так по нескольку сотен раз за двадцать секунд бега.
    //
    // Отсюда же брался и «заброс на крышу»: отброшенный назад игрок
    // оказывался внутри геометрии, спасательный подъём выталкивал его
    // вверх, а snapDown ставил на верх того блока, в котором он
    // застрял, — и так, кадр за кадром, по стене дома до конька.
    //
    // По вертикали такое отсечение уже стояло, и только по вертикали.
    // Это была половина правила: та же ошибка по горизонтали осталась
    // нетронутой и осталась видна.
    //
    // Правильное условие ровно одно: блок может остановить движение,
    // только если до этого кадра он был ВПЕРЕДИ коробки по этой оси.
    // Тогда упор может лишь укоротить шаг, но никогда не развернуть
    // его и не отбросить игрока дальше, чем он стоял в начале кадра.
    const f32 prevPos = pos[axis] - delta;
    const f32 prevMin = (axis == 1) ? prevPos : prevPos - box.halfWidth;
    const f32 prevMax = (axis == 1) ? prevPos + box.height
                                    : prevPos + box.halfWidth;

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 z = z0; z <= z1; ++z) {
            for (i32 x = x0; x <= x1; ++x) {
                if (!reg.isSolid(rd.at(x, y, z))) continue;
                const i32 c = (axis == 0) ? x : (axis == 1) ? y : z;
                if (delta > 0.f) {
                    // Ближняя грань блока — не позади коробки.
                    if ((f32)c < prevMax - EPS) continue;
                } else {
                    // Дальняя грань блока — не впереди коробки.
                    if ((f32)(c + 1) > prevMin + EPS) continue;
                }
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
