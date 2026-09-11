/**
 * @file spatial_hash.cpp
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#include "spatial_hash.h"
#include "../ecs/components.h"
#include <cmath>
#include <algorithm>

namespace combat {

void SpatialHash::rebuild(ecs::Registry& reg) {
    cells_.clear();
    entityCount_ = 0;
    age_ = 0;

    auto& hPool = reg.pool<ecs::Health>();
    for (usize i = 0; i < hPool.size(); ++i) {
        ecs::Entity e = hPool.entityAt((u32)i);
        auto* tf = reg.get<ecs::Transform>(e);
        if (!tf) continue;

        i32 cx = (i32)std::floor(tf->position.x / CELL_SIZE);
        i32 cz = (i32)std::floor(tf->position.z / CELL_SIZE);
        u64 key = cellKey(cx, cz);

        cells_[key].entities.push_back(e);
        ++entityCount_;
    }
}

void SpatialHash::queryRadiusXZ(const glm::vec3& center, f32 radius,
                                 std::vector<ecs::Entity>& out) const
{
    out.clear();
    if (radius <= 0.f) return;

    // Диапазон клеток, попадающих в радиус.
    i32 cx0 = (i32)std::floor((center.x - radius) / CELL_SIZE);
    i32 cx1 = (i32)std::floor((center.x + radius) / CELL_SIZE);
    i32 cz0 = (i32)std::floor((center.z - radius) / CELL_SIZE);
    i32 cz1 = (i32)std::floor((center.z + radius) / CELL_SIZE);

    const f32 r2 = radius * radius;

    // Собираем через set-подобный фильтр (без дублей не бывает,
    // потому что каждая сущность в одной клетке).
    for (i32 cz = cz0; cz <= cz1; ++cz) {
        for (i32 cx = cx0; cx <= cx1; ++cx) {
            auto it = cells_.find(cellKey(cx, cz));
            if (it == cells_.end()) continue;
            for (ecs::Entity e : it->second.entities) {
                out.push_back(e);
            }
        }
    }
    (void)r2;
    // Фильтрация по реальной дистанции — на стороне вызывающего
    // (там уже есть Transform).
}

void SpatialHash::queryAABB(const glm::vec3& mn, const glm::vec3& mx,
                             std::vector<ecs::Entity>& out) const
{
    out.clear();

    i32 cx0 = (i32)std::floor(mn.x / CELL_SIZE);
    i32 cx1 = (i32)std::floor(mx.x / CELL_SIZE);
    i32 cz0 = (i32)std::floor(mn.z / CELL_SIZE);
    i32 cz1 = (i32)std::floor(mx.z / CELL_SIZE);

    for (i32 cz = cz0; cz <= cz1; ++cz) {
        for (i32 cx = cx0; cx <= cx1; ++cx) {
            auto it = cells_.find(cellKey(cx, cz));
            if (it == cells_.end()) continue;
            for (ecs::Entity e : it->second.entities) {
                out.push_back(e);
            }
        }
    }
}

} // namespace combat
