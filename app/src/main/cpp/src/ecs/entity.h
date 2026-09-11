#pragma once
#include "../core/types.h"
#include <functional>

namespace ecs {

// ============================================================
// Дескриптор сущности: индекс + поколение.
// Поколение защищает от «висячих» дескрипторов: после destroy()
// старый Entity перестаёт проходить Registry::alive(), даже если
// индекс уже переиспользован новой сущностью.
//
// Вынесен в отдельный заголовок, чтобы компоненты (components.h)
// могли хранить ссылки на сущности, не подтягивая весь Registry.
// ============================================================
struct Entity {
    u32 id  = 0;
    u32 gen = 0;

    constexpr Entity() = default;
    constexpr explicit Entity(u32 i, u32 g = 0) : id(i), gen(g) {}

    /// Индекс 0 зарезервирован под «нет сущности».
    constexpr bool valid() const { return id != 0; }
    constexpr operator u32() const { return id; }
    constexpr bool operator==(Entity o) const { return id == o.id && gen == o.gen; }
    constexpr bool operator!=(Entity o) const { return !(*this == o); }
};

} // namespace ecs

namespace std {
template<> struct hash<ecs::Entity> {
    size_t operator()(ecs::Entity e) const noexcept {
        return (size_t)((u64)e.id * 0x9E3779B97F4A7C15ULL) ^ (size_t)e.gen;
    }
};
} // namespace std
