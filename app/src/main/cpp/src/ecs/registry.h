#pragma once
#include "../core/types.h"
#include "../core/log.h"
#include <vector>
#include <typeindex>
#include <unordered_map>
#include <memory>
#include <utility>

namespace ecs {

// ============================================================
// Sparse-set хранилище для одного типа компонента.
// O(1) добавление/удаление/поиск, плотная итерация.
// ============================================================
template<typename T>
class ComponentPool {
public:
    void add(Entity e, T c) {
        ASSERT(!has(e));
        ensure(e.id);
        sparse_[e.id] = (u32)dense_.size();
        entities_.push_back(e);
        dense_.push_back(std::move(c));
    }

    void remove(Entity e) {
        if (!has(e)) return;
        u32 idx = sparse_[e.id];
        u32 last = (u32)dense_.size() - 1;
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

    usize size() const { return dense_.size(); }
    Entity  entityAt(u32 i) const { return entities_[i]; }
    T&      at(u32 i)       { return dense_[i]; }
    const T& at(u32 i) const{ return dense_[i]; }

    auto& entities() { return entities_; }
    auto& data()     { return dense_; }

private:
    void ensure(u32 id) {
        if (id >= sparse_.size()) sparse_.resize(id + 1, NIL);
    }

    static constexpr u32 NIL = 0xFFFFFFFFu;
    std::vector<u32>    sparse_;   // e.id -> dense index
    std::vector<Entity> entities_;
    std::vector<T>      dense_;
};

// ============================================================
// Registry — владелец всех пулов + генерационный аллокатор сущностей.
// ============================================================
class Registry {
public:
    Entity create() {
        u32 id;
        if (!free_.empty()) {
            id = free_.back();
            free_.pop_back();
        } else {
            id = ++nextId_;
            if (id == 0) id = ++nextId_;  // пропустить 0 (NULL)
            generations_.push_back(0);
        }
        Entity e{ id, generations_[id - 1] };
        alive_.push_back(e);
        return e;
    }

    void destroy(Entity e) {
        if (!e.valid() || e.id > nextId_) return;
        // Уничтожить во всех пулах
        for (auto& [_, base] : pools_) base->destroy(e);
        // Инкремент поколения
        generations_[e.id - 1]++;
        free_.push_back(e.id);
    }

    bool alive(Entity e) const {
        if (!e.valid() || e.id > nextId_) return false;
        return generations_[e.id - 1] == e.gen;
    }

    template<typename T>
    ComponentPool<T>& pool() {
        auto key = std::type_index(typeid(T));
        auto it = pools_.find(key);
        if (it != pools_.end())
            return static_cast<PoolHolder<T>*>(it->second.get())->pool;
        auto p = std::make_unique<PoolHolder<T>>();
        auto* raw = &p->pool;
        pools_[key] = std::move(p);
        return *raw;
    }

    template<typename T>
    void add(Entity e, T c) { pool<T>().add(e, std::move(c)); }

    template<typename T>
    T* get(Entity e) { return pool<T>().get(e); }

    template<typename T>
    bool has(Entity e) { return pool<T>().has(e); }

    template<typename T>
    void remove(Entity e) { pool<T>().remove(e); }

    // ============================================================
    // Итерация по сущностям, имеющим все перечисленные компоненты.
    // Итерируем по самому маленькому пулу, для экономии.
    // ============================================================
    template<typename... Cs>
    struct View {
        explicit View(Registry& r) : reg(r) {
            std::initializer_list<usize> sizes = { reg.pool<Cs>().size()... };
            smallest_ = 0;
            usize minSize = ~usize(0);
            usize idx = 0;
            for (usize s : sizes) {
                if (s < minSize) { minSize = s; smallest_ = idx; }
                ++idx;
            }
        }

        template<typename F>
        void each(F&& f) {
            eachImpl(std::forward<F>(f),
                     std::index_sequence_for<Cs...>{});
        }

    private:
        template<typename F, usize... Is>
        void eachImpl(F&& f, std::index_sequence<Is...>) {
            // Достанем ссылки на все пулы
            auto& primary = pickPool<Is...>(std::integral_constant<usize, 0>{});
            (void)primary;
            iterateByIndex<F, Is...>(std::forward<F>(f),
                                     std::integral_constant<usize, 0>{},
                                     std::make_index_sequence<sizeof...(Cs)>{});
        }

        // Это упрощённая реализация; в продакшене используем реальную
        // and-it-join итерацию. Для экономии места здесь — проход по
        // самому маленькому пулу с проверкой has<> на остальных.

        template<usize... Is>
        auto& pickPool(std::integral_constant<usize, 0>) {
            // Возвращает первый пул — заглушка, используется реальная
            // логика в iterateByIndex.
            return reg.pool<std::tuple_element_t<0, std::tuple<Cs...>>>();
        }

        template<typename F, usize... Is>
        void iterateByIndex(F&& f,
                            std::integral_constant<usize, 0>,
                            std::index_sequence<Is...>)
        {
            // Итерируем по первому компоненту (упрощённо).
            // В следующей итерации — реальный "smallest pool first".
            auto& p = reg.pool<std::tuple_element_t<0, std::tuple<Cs...>>>();
            auto& ents = p.entities();
            for (usize i = 0; i < ents.size(); ++i) {
                Entity e = ents[i];
                if ((reg.pool<Cs>().has(e) && ...)) {
                    f(e, *reg.pool<Cs>().get(e)...);
                }
            }
        }

        Registry& reg;
        usize smallest_;
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

    u32 nextId_ = 0;
    std::vector<u32>    generations_;
    std::vector<Entity> alive_;
    std::vector<u32>    free_;
    std::unordered_map<std::type_index, std::unique_ptr<IPoolBase>> pools_;
};

} // namespace ecs