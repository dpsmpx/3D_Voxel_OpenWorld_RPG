/**
 * @file registry.h
 * @brief Entity-Component-System: дескрипторы сущностей, sparse-set пулы, реестр.
 */
#pragma once
#include "../core/types.h"
#include "../core/log.h"
#include "entity.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <array>
#include <utility>
#include <functional>

namespace ecs {

// ============================================================
// Идентификаторы типов компонентов без RTTI.
// Сборка идёт с -fno-rtti (см. CMakeLists.txt), поэтому typeid
// использовать нельзя; вместо него — счётчик, закреплённый за
// типом через локальную статику шаблона.
// ============================================================
namespace detail {
inline u32 nextComponentTypeId() {
    static u32 counter = 0;
    return counter++;
}
} // namespace detail

template<typename T>
inline u32 componentTypeId() {
    static const u32 id = detail::nextComponentTypeId();
    return id;
}

/// Sparse-set хранилище одного типа компонента.
/// O(1) вставка/удаление/поиск, плотная итерация.
template<typename T>
class ComponentPool {
public:
    /// Добавляет компонент; повторный вызов перезаписывает существующий.
    void add(Entity e, T c) {
        if (has(e)) { *get(e) = std::move(c); return; }
        ensure(e.id);
        sparse_[e.id] = (u32)dense_.size();
        entities_.push_back(e);
        dense_.push_back(std::move(c));
    }

    void remove(Entity e) {
        if (!has(e)) return;
        const u32 idx  = sparse_[e.id];
        const u32 last = (u32)dense_.size() - 1;
        if (idx != last) {
            dense_[idx]    = std::move(dense_[last]);
            entities_[idx] = entities_[last];
            sparse_[entities_[idx].id] = idx;
        }
        dense_.pop_back();
        entities_.pop_back();
        sparse_[e.id] = NIL;
    }

    T* get(Entity e) {
        if (!has(e)) return nullptr;
        return &dense_[sparse_[e.id]];
    }

    const T* get(Entity e) const {
        if (!has(e)) return nullptr;
        return &dense_[sparse_[e.id]];
    }

    bool has(Entity e) const {
        return e.id < sparse_.size() && sparse_[e.id] != NIL;
    }

    usize    size() const { return dense_.size(); }
    Entity   entityAt(u32 i) const { return entities_[i]; }
    T&       at(u32 i)       { return dense_[i]; }
    const T& at(u32 i) const { return dense_[i]; }

    const std::vector<Entity>& entities() const { return entities_; }
    std::vector<T>&            data()           { return dense_; }

private:
    void ensure(u32 id) {
        if (id >= sparse_.size()) sparse_.resize(id + 1, NIL);
    }

    static constexpr u32 NIL = 0xFFFFFFFFu;
    std::vector<u32>    sparse_;   // e.id -> индекс в dense_
    std::vector<Entity> entities_;
    std::vector<T>      dense_;
};

/// Registry — владелец всех пулов и генератор дескрипторов.
/// Не потокобезопасен: используется из игрового потока, фоновые
/// задачи работают с копиями данных.
class Registry {
public:
    /// Создаёт сущность, переиспользуя освободившийся индекс.
    Entity create() {
        u32 id;
        if (!free_.empty()) {
            id = free_.back();
            free_.pop_back();
        } else {
            id = (u32)generations_.size() + 1;   // индекс 0 зарезервирован
            generations_.push_back(0);
        }
        ++aliveCount_;
        return Entity{ id, generations_[id - 1] };
    }

    /// Удаляет сущность и все её компоненты.
    /// Повторный вызов со старым дескриптором ничего не делает.
    void destroy(Entity e) {
        if (!alive(e)) return;
        for (auto& p : pools_) if (p) p->destroy(e);
        ++generations_[e.id - 1];
        free_.push_back(e.id);
        --aliveCount_;
    }

    bool alive(Entity e) const {
        if (!e.valid() || e.id > generations_.size()) return false;
        return generations_[e.id - 1] == e.gen;
    }

    /// Восстанавливает дескриптор по «голому» индексу: часть подсистем
    /// хранит сущности как u32. Возвращает невалидный Entity, если
    /// индекс не выдавался.
    Entity fromId(u32 id) const {
        if (id == 0 || id > generations_.size()) return Entity{};
        return Entity{ id, generations_[id - 1] };
    }

    usize aliveCount() const { return aliveCount_; }

    template<typename T>
    ComponentPool<T>& pool() {
        const u32 tid = componentTypeId<T>();
        if (tid >= pools_.size()) pools_.resize(tid + 1);
        if (!pools_[tid]) pools_[tid] = std::make_unique<PoolHolder<T>>();
        return static_cast<PoolHolder<T>*>(pools_[tid].get())->pool;
    }

    template<typename T>
    void add(Entity e, T c) { pool<T>().add(e, std::move(c)); }

    template<typename T>
    T* get(Entity e) { return pool<T>().get(e); }

    template<typename T>
    bool has(Entity e) { return pool<T>().has(e); }

    template<typename T>
    void remove(Entity e) { pool<T>().remove(e); }

    /// Перегрузки для подсистем, хранящих сущность как «голый» u32
    /// (урон, снаряды, диалоги, UI, сохранения). Индекс поднимается
    /// до полноценного дескриптора текущего поколения.
    ///
    /// Внимание: если индекс успел быть переиспользован, обращение
    /// попадёт в новую сущность. Там, где это важно, храните Entity.
    template<typename T> T*   get(u32 id)        { return get<T>(fromId(id)); }
    template<typename T> bool has(u32 id)        { return has<T>(fromId(id)); }
    template<typename T> void remove(u32 id)     { remove<T>(fromId(id)); }
    template<typename T> void add(u32 id, T c)   { add<T>(fromId(id), std::move(c)); }
    void destroy(u32 id)                         { destroy(fromId(id)); }
    bool alive(u32 id) const                     { return alive(fromId(id)); }

    /// View — обход сущностей, имеющих все перечисленные компоненты.
    /// Ведущим берётся наименьший пул, остальные проверяются через
    /// has<>. Список сущностей копируется, поэтому callback может
    /// безопасно добавлять и удалять компоненты по ходу обхода.
    template<typename... Cs>
    class View {
    public:
        explicit View(Registry& r) : reg_(r) {}

        template<typename F>
        void each(F&& f) {
            static_assert(sizeof...(Cs) > 0, "View требует хотя бы один компонент");
            const std::array<usize, sizeof...(Cs)> sizes{ reg_.pool<Cs>().size()... };
            usize lead = 0;
            for (usize i = 1; i < sizes.size(); ++i)
                if (sizes[i] < sizes[lead]) lead = i;

            snapshot_.clear();
            usize idx = 0;
            (collectIf<Cs>(idx++, lead), ...);

            for (Entity e : snapshot_) {
                if ((reg_.pool<Cs>().has(e) && ...))
                    f(e, *reg_.pool<Cs>().get(e)...);
            }
        }

    private:
        template<typename C>
        void collectIf(usize idx, usize lead) {
            if (idx != lead) return;
            const auto& ents = reg_.pool<C>().entities();
            snapshot_.assign(ents.begin(), ents.end());
        }

        Registry&           reg_;
        std::vector<Entity> snapshot_;
    };

    template<typename... Cs>
    View<Cs...> view() { return View<Cs...>(*this); }

private:
    struct IPoolBase {
        virtual ~IPoolBase() = default;
        virtual void destroy(Entity e) = 0;
    };
    template<typename T>
    struct PoolHolder : IPoolBase {
        ComponentPool<T> pool;
        void destroy(Entity e) override { pool.remove(e); }
    };

    std::vector<u32>                        generations_;
    std::vector<u32>                        free_;
    std::vector<std::unique_ptr<IPoolBase>> pools_;
    usize                                   aliveCount_ = 0;
};

} // namespace ecs
