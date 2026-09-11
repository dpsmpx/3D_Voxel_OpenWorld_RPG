#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>

namespace combat {

// ============================================================
// Spatial hash для быстрого поиска сущностей по позиции.
//
// Разбивает мир на клетки размером CELL_SIZE. Каждая клетка
// хранит список индексов сущностей. Используется в hit-detection,
// чтобы не сканировать все сущности каждый удар.
//
// Обновляется лениво — если с момента последнего rebuild
// прошло больше CACHE_LIFETIME кадров, перестраивается.
// ============================================================
class SpatialHash {
public:
    static constexpr f32 CELL_SIZE = 4.0f;
    static constexpr i32 CACHE_LIFETIME = 15;   // кадров

    // Полная перестройка по реестру.
    // Собирает все сущности с Health + Transform.
    void rebuild(ecs::Registry& reg);

    // Пометить как «устаревший» — следующий запрос обновится.
    void invalidate() { age_ = CACHE_LIFETIME + 1; }

    // Проверить устаревание и перестроить при необходимости.
    void ensureFresh(ecs::Registry& reg) {
        if (age_ > CACHE_LIFETIME) rebuild(reg);
    }

    // Увеличить счётчик возраста — вызывать раз в кадр.
    void tick() { if (age_ <= CACHE_LIFETIME) ++age_; }

    // Найти все сущности в радиусе (2D по XZ, игнорируя Y).
    // Возвращает список в out.
    void queryRadiusXZ(const glm::vec3& center, f32 radius,
                       std::vector<ecs::Entity>& out) const;

    // Найти все сущности в кубе (AABB по 3D).
    void queryAABB(const glm::vec3& mn, const glm::vec3& mx,
                   std::vector<ecs::Entity>& out) const;

    usize entityCount() const { return entityCount_; }
    usize cellCount() const   { return cells_.size(); }

private:
    // Ключ клетки: (cx, cz) упакованные в u64.
    static u64 cellKey(i32 cx, i32 cz) {
        return ((u64)(u32)cx << 32) | (u64)(u32)cz;
    }

    struct Cell {
        std::vector<ecs::Entity> entities;
    };

    std::unordered_map<u64, Cell> cells_;
    usize entityCount_ = 0;
    i32   age_ = CACHE_LIFETIME + 1;
};

} // namespace combat