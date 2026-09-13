/**
 * @file pathfinding.cpp
 * @brief Навигация по воксельной сетке: поиск пути A*.
 */
#include "pathfinding.h"
#include "../block.h"
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <cmath>
#include <algorithm>
#include <vector>

namespace world::ai {

namespace {

constexpr i32 NEIGHBOR_DX[4] = { 1, -1, 0, 0 };
constexpr i32 NEIGHBOR_DZ[4] = { 0, 0, 1, -1 };

inline u64 hashCell(i32 x, i32 y, i32 z) {
    u64 h = (u64)(u32)x * 0x9E3779B97F4A7C15ULL;
    h ^= (u64)(u32)y * 0xC4CEB9FE1A85EC53ULL;
    h ^= (u64)(u32)z * 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return h;
}

struct OpenNode {
    f32 g, f;
    glm::ivec3 pos;
    /// Индекс САМОГО узла в nodeStore, а не его родителя. Поле
    /// называлось parentIdx и хранило при этом собственный индекс —
    /// из-за этой путаницы восстановление пути и оказалось неверным.
    i32 selfIdx;
};
struct OpenNodeCmp {
    bool operator()(const OpenNode& a, const OpenNode& b) const { return a.f > b.f; }
};

// ============================================================
// Проверка столбца: пусто ли на высотах [y, y+bodyHeight)
// ============================================================
bool columnFree(world::VoxelReader& rd, i32 x, i32 z, i32 yBase, f32 bodyHeight) {
    i32 yTop = yBase + (i32)std::ceil(bodyHeight);
    auto& reg = world::blocks();
    for (i32 y = yBase; y < yTop; ++y) {
        if (reg.isSolid(rd.at(x, y, z))) return false;
    }
    return true;
}

bool groundSolid(world::VoxelReader& rd, i32 x, i32 z, i32 yBase) {
    auto& reg = world::blocks();
    return reg.isSolid(rd.at(x, yBase - 1, z));
}

} // namespace

bool isStandable(world::VoxelReader& rd, i32 x, i32 y, i32 z, const MoveParams& mp) {
    if (y < 1 || y >= world::CHUNK_SIZE_Y - 2) return false;

    // Тело должно помещаться
    if (!columnFree(rd, x, z, y, mp.bodyHeight)) return false;

    // Опора снизу
    if (groundSolid(rd, x, z, y)) return true;

    // Разрешим "висеть" в воде/лаве
    if (mp.allowWater) {
        u16 below = rd.at(x, y - 1, z);
        if (below == world::WATER) return true;
    }
    return false;
}

bool canStep(world::VoxelReader& rd,
             i32 x, i32 y, i32 z,
             i32 nx, i32 ny, i32 nz,
             const MoveParams& mp,
             f32& stepCost)
{
    const i32 dx = nx - x, dy = ny - y, dz = nz - z;

    if (dx == 0 && dz == 0 && dy == 0) return false;
    if (std::abs(dx) > 1 || std::abs(dz) > 1) return false;
    if (std::abs(dy) > 1) {
        // Только падение (не прыжок вверх на 2+)
        if (dy > 0) return false;
        if (!mp.allowFall) return false;
    }

    // Целевая клетка должна быть standable
    if (!isStandable(rd, nx, ny, nz, mp)) return false;

    // Для горизонтального шага без изменения Y проверяем, что можно
    // пройти "напрямую" (нет стены между)
    if (dy == 0) {
        stepCost = 1.0f;
        return true;
    }
    // Прыжок на 1 вверх
    if (dy == 1) {
        if (!mp.allowJump) return false;
        // В промежуточной позиции (nx, y+1, nz) должно быть свободно
        if (!columnFree(rd, nx, nz, y + 1, mp.bodyHeight)) return false;
        stepCost = 1.6f;
        return true;
    }
    // Падение
    if (dy < 0) {
        if (-dy > mp.maxFallDepth) return false;
        // Проверим, что ниже не застрянет в блоке
        stepCost = 1.0f + (-dy) * 0.15f;
        return true;
    }
    return false;
}

namespace {
// Внутренняя куча для A* с реконструкцией пути
struct AStar {
    std::unordered_map<u64, f32> gScore;
    /// Клетка -> индекс её родителя в nodeStore. Именно родителя:
    /// по этой таблице путь и разматывается от цели к началу.
    std::unordered_map<u64, i32> parentIdx;
    std::unordered_set<u64>      closedSet;
    std::vector<glm::ivec3> nodeStore;
    std::priority_queue<OpenNode, std::vector<OpenNode>, OpenNodeCmp> open;
};
}

PathResult findPath(world::ChunkManager& world,
                    const glm::ivec3& start,
                    const glm::ivec3& goal,
                    const MoveParams& mp,
                    i32 maxIterations)
{
    PathResult out;
    if (start == goal) { out.ok = true; return out; }

    // Один курсор на весь поиск. A* с бюджетом в полторы тысячи шагов
    // делает тысячи чтений вокселей, и почти все — в одном и том же
    // чанке. Через ChunkManager::getVoxel каждое стоило поиска в
    // хэш-таблице и двух захватов замков; здесь чанк ищется только
    // при переходе через его границу.
    world::VoxelReader rd(world);

    AStar A;
    auto heur = [](const glm::ivec3& a, const glm::ivec3& b) {
        return std::fabs((f32)(a.x - b.x))
             + std::fabs((f32)(a.y - b.y)) * 1.2f
             + std::fabs((f32)(a.z - b.z));
    };

    A.nodeStore.push_back(start);
    u64 hStart = hashCell(start.x, start.y, start.z);
    A.gScore[hStart] = 0.f;
    A.parentIdx[hStart] = -1;   // у начала родителя нет
    A.open.push({ 0.f, heur(start, goal), start, 0 });

    i32 iterations = 0;

    while (!A.open.empty() && iterations < maxIterations) {
        ++iterations;
        OpenNode cur = A.open.top();
        A.open.pop();

        u64 hCur = hashCell(cur.pos.x, cur.pos.y, cur.pos.z);
        if (A.closedSet.count(hCur)) continue;
        A.closedSet.insert(hCur);

        if (cur.pos == goal) {
            // Разматываем путь от цели к началу по таблице родителей.
            //
            // Раньше здесь получалась чепуха: родителем новой клетки
            // записывался «предпоследний добавленный узел» вместо
            // текущей клетки, и цепочка уводила в соседнюю ветку
            // поиска. Путь в восемь блоков выходил из четырёх тысяч
            // точек, моб шёл по нему зигзагом, а сглаживание потом
            // перебирало эти тысячи точек попарно, проверяя видимость.
            std::vector<glm::ivec3> rev;
            rev.push_back(cur.pos);
            u64 h = hCur;
            // Узлов в цепочке не больше, чем было раскрыто клеток:
            // каждая попадает в неё не более одного раза. Ограничение
            // всё же оставляем — на случай, если это перестанет быть
            // правдой, зацикливаться здесь нельзя.
            const usize limit = A.nodeStore.size() + 1;
            while (rev.size() <= limit) {
                auto it = A.parentIdx.find(h);
                if (it == A.parentIdx.end() || it->second < 0) break;
                const glm::ivec3 p = A.nodeStore[(usize)it->second];
                rev.push_back(p);
                h = hashCell(p.x, p.y, p.z);
            }
            std::reverse(rev.begin(), rev.end());
            out.waypoints = std::move(rev);
            out.length = cur.f;
            out.ok = true;
            return out;
        }

        // Обход соседей. Проверяем 4 кардинала + прыжок + падение.
        const i32 cx = cur.pos.x, cy = cur.pos.y, cz = cur.pos.z;

        // Варианты y: same, +1, -1..-N
        auto tryNeighbor = [&](i32 nx, i32 ny, i32 nz) {
            f32 stepCost;
            if (!canStep(rd, cx, cy, cz, nx, ny, nz, mp, stepCost)) return;

            u64 hN = hashCell(nx, ny, nz);
            if (A.closedSet.count(hN)) return;

            f32 tentative = A.gScore[hCur] + stepCost;
            auto it = A.gScore.find(hN);
            if (it != A.gScore.end() && tentative >= it->second) return;

            A.gScore[hN] = tentative;
            const i32 newIdx = (i32)A.nodeStore.size();
            A.nodeStore.push_back({ nx, ny, nz });
            // Родитель новой клетки — та, из которой мы в неё шагнули,
            // то есть текущая. Её индекс в nodeStore лежит прямо в узле.
            A.parentIdx[hN] = cur.selfIdx;

            f32 f = tentative + heur({nx, ny, nz}, goal);
            A.open.push({ tentative, f, {nx, ny, nz}, newIdx });
        };

        for (int i = 0; i < 4; ++i) {
            i32 nx = cx + NEIGHBOR_DX[i];
            i32 nz = cz + NEIGHBOR_DZ[i];

            // 1. Прямой шаг по той же высоте
            tryNeighbor(nx, cy, nz);
            // 2. Шаг с прыжком вверх
            tryNeighbor(nx, cy + 1, nz);
            // 3. Падение на 1..3
            for (i32 d = 1; d <= 3; ++d)
                tryNeighbor(nx, cy - d, nz);
        }
    }

    return out;
}

// ============================================================
// Упрощение: линейная интерполяция через пустое пространство
// ============================================================
namespace {

bool hasLineOfSight(world::VoxelReader& rd,
                    const glm::ivec3& a,
                    const glm::ivec3& b,
                    const MoveParams& mp)
{
    // Bresenham по 3 осям, проверяем что в каждой точке тело проходит
    i32 dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    i32 steps = std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz)));
    if (steps == 0) return true;

    auto& reg = world::blocks();
    for (i32 i = 0; i <= steps; ++i) {
        f32 t = (f32)i / (f32)steps;
        i32 x = a.x + (i32)std::round(dx * t);
        i32 y = a.y + (i32)std::round(dy * t);
        i32 z = a.z + (i32)std::round(dz * t);
        if (y < 1 || y >= world::CHUNK_SIZE_Y - 1) return false;
        // тело
        i32 yTop = y + (i32)std::ceil(mp.bodyHeight);
        for (i32 yy = y; yy < yTop; ++yy)
            if (reg.isSolid(rd.at(x, yy, z))) return false;
        // под ногами (кроме конечной точки — там standable уже проверен)
        if (i < steps && !reg.isSolid(rd.at(x, y - 1, z))) return false;
    }
    return true;
}

} // namespace

void smoothPath(world::ChunkManager& world,
                std::vector<glm::ivec3>& path,
                const MoveParams& mp)
{
    if (path.size() < 3) return;
    world::VoxelReader rd(world);
    std::vector<glm::ivec3> out;
    out.push_back(path.front());
    usize i = 0;
    while (i + 2 < path.size()) {
        // Ищем самый дальний видимый узел
        bool found = false;
        for (usize k = path.size() - 1; k > i + 1; --k) {
            if (hasLineOfSight(rd, path[i], path[k], mp)) {
                out.push_back(path[k]);
                i = k;
                found = true;
                break;
            }
        }
        if (!found) {
            out.push_back(path[i + 1]);
            i = i + 1;
        }
    }
    if (out.back() != path.back()) out.push_back(path.back());
    path = std::move(out);
}

} // namespace world::ai
