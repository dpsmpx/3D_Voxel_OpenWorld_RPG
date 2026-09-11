/**
 * @file entity.h
 * @brief Entity-Component-System: дескрипторы сущностей, sparse-set пулы, реестр.
 */
#pragma once
#include "../core/types.h"

// EnTT по умолчанию не хранит пустые компоненты (empty type
// optimization): для тегов вроде PlayerTag storage<T>::get()
// возвращает void, а patch() зовёт функтор без аргументов.
// Из-за этого обёртка ComponentPool<T> перестала бы быть
// единообразной. Один байт на тег дешевле, чем ветвление
// if constexpr по всему адаптеру.
#ifndef ENTT_NO_ETO
#define ENTT_NO_ETO
#endif

#include <entt/entt.hpp>
#include <functional>

namespace ecs {

/// Тип сущности EnTT, на котором работает хранилище.
using EnttEntity = entt::entity;
using EnttTraits = entt::entt_traits<EnttEntity>;

// ============================================================
// Дескриптор сущности: индекс + поколение.
//
// Тонкая обёртка над entt::entity, разложенная на две части,
// чтобы подсистемы могли хранить сущность как «голый» u32
// (урон, снаряды, диалоги, сохранения) и при этом не терять
// защиту от висячих дескрипторов.
//
// Индекс 0 зарезервирован под «нет сущности», поэтому id на
// единицу больше индекса EnTT.
// ============================================================
struct Entity {
    u32 id  = 0;   ///< индекс EnTT + 1; 0 — «нет сущности»
    u32 gen = 0;   ///< поколение (version) EnTT

    constexpr Entity() = default;
    constexpr explicit Entity(u32 i, u32 g = 0) : id(i), gen(g) {}

    /// Создаёт дескриптор из сущности EnTT.
    static Entity fromEntt(EnttEntity e) {
        if (e == entt::null) return Entity{};
        return Entity{ (u32)EnttTraits::to_entity(e) + 1u,
                       (u32)EnttTraits::to_version(e) };
    }

    /// Обратное преобразование. Для невалидного дескриптора — entt::null.
    EnttEntity toEntt() const {
        if (id == 0) return entt::null;
        return EnttTraits::construct((EnttTraits::entity_type)(id - 1u),
                                     (EnttTraits::version_type)gen);
    }

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
