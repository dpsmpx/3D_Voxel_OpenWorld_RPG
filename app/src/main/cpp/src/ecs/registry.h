/**
 * @file registry.h
 * @brief Entity-Component-System: реестр поверх EnTT.
 */
#pragma once
#include "../core/types.h"
#include "../core/log.h"
#include "entity.h"
#include <entt/entt.hpp>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace ecs {

// ============================================================
// ComponentPool<T> — представление хранилища EnTT в терминах
// игрового кода: плотный массив компонентов с доступом по индексу.
//
// Собственного хранения нет: это тонкая обёртка над
// entt::storage<T>, которая переводит entt::entity в ecs::Entity
// и обратно. Порядок обхода — порядок EnTT.
// ============================================================
template<typename T>
class ComponentPool {
public:
    using Storage = entt::storage<T>;

    explicit ComponentPool(entt::registry& reg, Storage& st)
        : reg_(&reg), st_(&st) {}

    /// Добавляет компонент; повторный вызов перезаписывает существующий.
    void add(Entity e, T c) {
        const EnttEntity h = e.toEntt();
        if (!reg_->valid(h)) return;
        if (st_->contains(h)) st_->patch(h, [&](T& dst) { dst = std::move(c); });
        else                  st_->emplace(h, std::move(c));
    }

    void remove(Entity e) {
        const EnttEntity h = e.toEntt();
        if (st_->contains(h)) st_->erase(h);
    }

    T* get(Entity e) {
        const EnttEntity h = e.toEntt();
        return st_->contains(h) ? &st_->get(h) : nullptr;
    }

    const T* get(Entity e) const {
        const EnttEntity h = e.toEntt();
        return st_->contains(h) ? &st_->get(h) : nullptr;
    }

    bool has(Entity e) const { return st_->contains(e.toEntt()); }

    usize size() const { return st_->size(); }

    /// Сущность по плотному индексу [0, size()).
    Entity entityAt(u32 i) const { return Entity::fromEntt(st_->data()[i]); }

    /// Компонент по тому же плотному индексу.
    T&       at(u32 i)       { return st_->get(st_->data()[i]); }
    const T& at(u32 i) const { return st_->get(st_->data()[i]); }

    /// Снимок списка сущностей. Копия, а не ссылка: обход можно
    /// безопасно совмещать с добавлением и удалением компонентов.
    std::vector<Entity> entities() const {
        std::vector<Entity> out;
        out.reserve(st_->size());
        for (usize i = 0; i < st_->size(); ++i) out.push_back(entityAt((u32)i));
        return out;
    }

private:
    entt::registry* reg_;
    Storage*        st_;
};

// ============================================================
// Registry — владелец сущностей и компонентов.
//
// Хранение и итерацию выполняет EnTT (требование ТЗ 3.2);
// этот класс даёт игровому коду привычный интерфейс и
// перегрузки для подсистем, хранящих сущность как u32.
//
// Не потокобезопасен: используется из игрового потока, фоновые
// задачи работают с копиями данных.
// ============================================================
class Registry {
public:
    Entity create() { return Entity::fromEntt(reg_.create()); }

    /// Удаляет сущность и все её компоненты.
    /// Повторный вызов со старым дескриптором ничего не делает.
    void destroy(Entity e) {
        const EnttEntity h = e.toEntt();
        if (reg_.valid(h)) reg_.destroy(h);
    }

    bool alive(Entity e) const { return reg_.valid(e.toEntt()); }

    /// Восстанавливает дескриптор по «голому» индексу.
    /// Текущее поколение берётся у реестра: вызывающий хранил
    /// только индекс. Возвращает невалидный Entity, если сущности нет.
    Entity fromId(u32 id) const {
        if (id == 0) return Entity{};
        const auto idx = (EnttTraits::entity_type)(id - 1u);

        // registry::current отдаёт поколение, закреплённое сейчас за
        // индексом. Для никогда не выдававшегося или освобождённого
        // индекса это версия-надгробие.
        const EnttEntity probe = EnttTraits::construct(idx, 0);
        const auto version = reg_.current(probe);
        if (version == EnttTraits::to_version(entt::tombstone)) return Entity{};

        const EnttEntity actual = EnttTraits::construct(idx, version);
        if (!reg_.valid(actual)) return Entity{};
        return Entity::fromEntt(actual);
    }

    /// Уничтожает всё, кроме одной сущности.
    ///
    /// Спрашивает смена мира: мобы, жители, лут и снаряды принадлежат
    /// ТОМУ миру, а игрок переходит в новый вместе со всем, что у
    /// него в карманах. Удалять по одному типу компонента нельзя:
    /// сущность без Transform (а такие есть) осталась бы жить.
    void destroyAllExcept(Entity keep) {
        const EnttEntity k = keep.toEntt();

        // Сначала собрать, потом рушить: разрушение сущности правит
        // то самое хранилище, по которому идёт обход.
        auto& storage = reg_.template storage<EnttEntity>();
        std::vector<EnttEntity> doomed;
        doomed.reserve(storage.free_list());
        for (auto h : storage) if (h != k) doomed.push_back(h);
        for (auto h : doomed) if (reg_.valid(h)) reg_.destroy(h);
    }

    usize aliveCount() const {
        const auto* storage = reg_.storage<EnttEntity>();
        return storage ? storage->free_list() : 0;
    }

    /// Хранилище компонентов данного типа.
    ///
    /// Возвращается ссылка на закэшированный дескриптор: игровой код
    /// повсеместно пишет `auto& pool = reg.pool<T>()`, а сам дескриптор
    /// — это пара указателей, создавать его каждый раз незачем.
    template<typename T>
    ComponentPool<T>& pool() {
        const u32 tid = componentTypeId<T>();
        if (tid >= poolCache_.size()) poolCache_.resize(tid + 1);
        if (!poolCache_[tid]) {
            poolCache_[tid] = std::make_unique<PoolHolder<T>>(
                ComponentPool<T>(reg_, reg_.storage<T>()));
        }
        return static_cast<PoolHolder<T>*>(poolCache_[tid].get())->pool;
    }

    template<typename T>
    void add(Entity e, T c) { pool<T>().add(e, std::move(c)); }

    template<typename T>
    T* get(Entity e) {
        const EnttEntity h = e.toEntt();
        if (!reg_.valid(h)) return nullptr;
        return reg_.try_get<T>(h);
    }

    template<typename T>
    bool has(Entity e) {
        const EnttEntity h = e.toEntt();
        return reg_.valid(h) && reg_.all_of<T>(h);
    }

    template<typename T>
    void remove(Entity e) {
        const EnttEntity h = e.toEntt();
        if (reg_.valid(h)) reg_.remove<T>(h);
    }

    // ------------------------------------------------------------
    // Перегрузки для подсистем, хранящих сущность как «голый» u32
    // (урон, снаряды, диалоги, UI, сохранения).
    //
    // Внимание: индекс не несёт поколения, поэтому обращение по
    // нему попадёт в новую сущность, если слот переиспользован.
    // Там, где это важно, храните Entity.
    // ------------------------------------------------------------
    template<typename T> T*   get(u32 id)      { return get<T>(fromId(id)); }
    template<typename T> bool has(u32 id)      { return has<T>(fromId(id)); }
    template<typename T> void remove(u32 id)   { remove<T>(fromId(id)); }
    template<typename T> void add(u32 id, T c) { add<T>(fromId(id), std::move(c)); }
    void destroy(u32 id)                       { destroy(fromId(id)); }
    bool alive(u32 id) const                   { return alive(fromId(id)); }

    // ============================================================
    // View — обход сущностей, имеющих все перечисленные компоненты.
    // Пересечение наборов выполняет EnTT; список копируется, чтобы
    // callback мог безопасно добавлять и удалять компоненты.
    // ============================================================
    template<typename... Cs>
    class View {
    public:
        explicit View(entt::registry& r) : reg_(&r) {}

        template<typename F>
        void each(F&& f) {
            static_assert(sizeof...(Cs) > 0, "View требует хотя бы один компонент");
            std::vector<EnttEntity> snapshot;
            auto v = reg_->view<Cs...>();
            // У представления по ОДНОМУ компоненту размер точный и
            // зовётся size(); size_hint() есть только у пересечения
            // нескольких. Без этой развилки view<T>().each(...) не
            // компилировался вовсе — а это самый обычный случай.
            if constexpr (sizeof...(Cs) == 1) snapshot.reserve(v.size());
            else                              snapshot.reserve(v.size_hint());
            for (auto h : v) snapshot.push_back(h);

            for (auto h : snapshot) {
                if (!reg_->valid(h)) continue;
                if (!reg_->all_of<Cs...>(h)) continue;
                f(Entity::fromEntt(h), reg_->get<Cs>(h)...);
            }
        }

    private:
        entt::registry* reg_;
    };

    template<typename... Cs>
    View<Cs...> view() { return View<Cs...>(reg_); }

    /// Прямой доступ к реестру EnTT — для кода, которому нужны
    /// возможности библиотеки сверх этой обёртки.
    entt::registry&       raw()       { return reg_; }
    const entt::registry& raw() const { return reg_; }

private:
    /// Идентификаторы типов компонентов без RTTI: сборка идёт с
    /// -fno-rtti, поэтому typeid использовать нельзя.
    struct IPoolHolder { virtual ~IPoolHolder() = default; };
    template<typename T>
    struct PoolHolder : IPoolHolder {
        explicit PoolHolder(ComponentPool<T> p) : pool(std::move(p)) {}
        ComponentPool<T> pool;
    };

    static u32 nextComponentTypeId() {
        static u32 counter = 0;
        return counter++;
    }
    template<typename T>
    static u32 componentTypeId() {
        static const u32 id = nextComponentTypeId();
        return id;
    }

    entt::registry                            reg_;
    std::vector<std::unique_ptr<IPoolHolder>> poolCache_;
};

} // namespace ecs
