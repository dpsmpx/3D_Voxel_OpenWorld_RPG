/**
 * @file memory.h
 * @brief Ядро движка: базовые типы, математика, планировщик задач, аллокаторы.
 */
#pragma once
#include "types.h"
#include "log.h"
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

namespace mem {

/// Arena — bump-аллокатор блоками фиксированного размера.
/// Подходит для покадровых данных и временных буферов генерации:
/// выделение — сдвиг указателя, освобождение — целиком через reset().
///
/// Деструкторы объектов, созданных через make<T>(), НЕ вызываются.
/// Кладите в арену только тривиально разрушаемые типы.
class Arena {
public:
    explicit Arena(usize blockSize = 1u << 20)
        : blockSize_(blockSize < 64 ? 64 : blockSize) {}

    ~Arena() { releaseAll(); }

    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& o) noexcept
        : blockSize_(o.blockSize_), blocks_(std::move(o.blocks_)),
          current_(o.current_), offset_(o.offset_)
    {
        o.blocks_.clear();
        o.current_ = 0;
        o.offset_  = 0;
    }

    Arena& operator=(Arena&& o) noexcept {
        if (this != &o) {
            releaseAll();
            blockSize_ = o.blockSize_;
            blocks_    = std::move(o.blocks_);
            current_   = o.current_;
            offset_    = o.offset_;
            o.blocks_.clear();
            o.current_ = 0;
            o.offset_  = 0;
        }
        return *this;
    }

    /// Выделяет size байт с указанным выравниванием.
    /// Возвращает nullptr, только если не хватило системной памяти.
    void* alloc(usize size, usize align = alignof(std::max_align_t)) {
        if (size == 0) return nullptr;
        ASSERT_MSG((align & (align - 1)) == 0, "align должен быть степенью двойки");

        // Запрос крупнее блока обслуживается отдельным блоком под него.
        if (size + align > blockSize_) return allocOversized(size, align);

        if (blocks_.empty() && !pushBlock(blockSize_)) return nullptr;

        const usize mask    = align - 1;
        usize       aligned = (offset_ + mask) & ~mask;

        if (aligned + size > blocks_[current_].size) {
            if (!advanceBlock()) return nullptr;
            aligned = (offset_ + mask) & ~mask;
        }

        void* p = blocks_[current_].data + aligned;
        offset_ = aligned + size;
        return p;
    }

    /// Конструирует T в арене. Деструктор вызван не будет.
    template<typename T, typename... Args>
    T* make(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena не вызывает деструкторы");
        void* p = alloc(sizeof(T), alignof(T));
        return p ? new (p) T(std::forward<Args>(args)...) : nullptr;
    }

    /// Переиспользует уже выделенные блоки с начала. Память не возвращается ОС.
    void reset() {
        current_ = 0;
        offset_  = 0;
        // Блоки нестандартного размера (oversized) больше не нужны.
        while (blocks_.size() > 1 && blocks_.back().size != blockSize_) {
            ::free(blocks_.back().data);
            blocks_.pop_back();
        }
    }

    usize totalBytes() const {
        usize n = 0;
        for (const auto& b : blocks_) n += b.size;
        return n;
    }
    usize blockCount() const { return blocks_.size(); }

private:
    struct Block { u8* data; usize size; };

    bool pushBlock(usize size) {
        void* p = ::malloc(size);
        if (!p) { LOGE("Arena: не хватило памяти на блок %zu байт", size); return false; }
        blocks_.push_back(Block{ (u8*)p, size });
        current_ = blocks_.size() - 1;
        offset_  = 0;
        return true;
    }

    bool advanceBlock() {
        // Переходим к следующему уже существующему блоку подходящего размера…
        for (usize i = current_ + 1; i < blocks_.size(); ++i) {
            if (blocks_[i].size >= blockSize_) {
                current_ = i;
                offset_  = 0;
                return true;
            }
        }
        // …или выделяем новый.
        return pushBlock(blockSize_);
    }

    void* allocOversized(usize size, usize align) {
        const usize total = size + align;
        void* p = ::malloc(total);
        if (!p) { LOGE("Arena: не хватило памяти на %zu байт", total); return nullptr; }
        blocks_.push_back(Block{ (u8*)p, total });
        const usize mask = align - 1;
        return (void*)(((usize)p + mask) & ~mask);
    }

    void releaseAll() {
        for (auto& b : blocks_) ::free(b.data);
        blocks_.clear();
        current_ = 0;
        offset_  = 0;
    }

    usize              blockSize_;
    std::vector<Block> blocks_;
    usize              current_ = 0;
    usize              offset_  = 0;
};

/// Pool<T> — пул объектов со стабильными адресами.
/// Хранилище растёт чанками, поэтому выданные указатели остаются
/// валидными после расширения (в отличие от std::vector).
template<typename T>
class Pool {
public:
    explicit Pool(usize chunkCapacity = 1024)
        : chunkCapacity_(chunkCapacity < 16 ? 16 : chunkCapacity) {}

    ~Pool() { clear(); }

    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;

    /// Возвращает сконструированный объект или nullptr при нехватке памяти.
    template<typename... Args>
    T* alloc(Args&&... args) {
        if (free_.empty() && !grow()) return nullptr;
        T* slot = free_.back();
        free_.pop_back();
        ++liveCount_;
        return new (slot) T(std::forward<Args>(args)...);
    }

    /// Разрушает объект и возвращает слот в пул.
    void free(T* p) {
        if (!p) return;
        p->~T();
        free_.push_back(p);
        --liveCount_;
    }

    usize liveCount() const { return liveCount_; }
    usize capacity()  const { return chunks_.size() * chunkCapacity_; }

    /// Возвращает все слоты в пул. Живые объекты НЕ разрушаются —
    /// вызывайте только когда все они уже освобождены.
    void clear() {
        for (auto& c : chunks_) ::operator delete[](c, std::align_val_t{alignof(T)});
        chunks_.clear();
        free_.clear();
        liveCount_ = 0;
    }

private:
    bool grow() {
        void* raw = ::operator new[](sizeof(T) * chunkCapacity_,
                                     std::align_val_t{alignof(T)},
                                     std::nothrow);
        if (!raw) { LOGE("Pool: не хватило памяти на чанк"); return false; }
        chunks_.push_back(raw);
        T* base = (T*)raw;
        free_.reserve(free_.size() + chunkCapacity_);
        // В обратном порядке, чтобы back() отдавал слоты по возрастанию.
        for (usize i = chunkCapacity_; i > 0; --i) free_.push_back(base + (i - 1));
        return true;
    }

    usize              chunkCapacity_;
    std::vector<void*> chunks_;   // сырая память, объекты создаются вручную
    std::vector<T*>    free_;
    usize              liveCount_ = 0;
};

/// ArenaResource — адаптер Arena к std::pmr (требование ТЗ 3.2).
///
/// Позволяет отдавать арену стандартным контейнерам:
///
///     mem::Arena arena;
///     mem::ArenaResource res{arena};
///     std::pmr::vector<Quad> quads{&res};
///
/// Все выделения контейнера идут bump-указателем, освобождение —
/// одним arena.reset() в конце кадра. Освобождение поэлементно
/// не делается: deallocate — пустая операция, это и есть смысл
/// арены.
class ArenaResource final : public std::pmr::memory_resource {
public:
    explicit ArenaResource(Arena& arena) : arena_(&arena) {}

private:
    void* do_allocate(usize bytes, usize align) override {
        void* p = arena_->alloc(bytes, align);
        // Сигнатура обязывает вернуть валидный указатель; при OOM
        // честнее упасть здесь, чем отдать nullptr в контейнер.
        ASSERT_MSG(p, "ArenaResource: арена исчерпана");
        return p;
    }

    void do_deallocate(void*, usize, usize) override {
        // Арена освобождается целиком через Arena::reset().
    }

    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
        // Сравниваем по адресу: dynamic_cast недоступен, сборка идёт
        // с -fno-rtti. Два ресурса взаимозаменяемы только если это
        // один и тот же объект — для арены этого достаточно.
        return this == &o;
    }

    Arena* arena_;
};

/// FrameAllocator — две арены с чередованием.
/// Данные, выделенные в кадре N, живут до конца кадра N+1: этого
/// хватает, чтобы рендер дочитал то, что подготовил апдейт.
class FrameAllocator {
public:
    void beginFrame() {
        std::swap(cur_, prev_);
        cur_.reset();
    }

    void* alloc(usize size, usize align = alignof(std::max_align_t)) {
        return cur_.alloc(size, align);
    }

    template<typename T, typename... Args>
    T* make(Args&&... args) { return cur_.make<T>(std::forward<Args>(args)...); }

    /// Ресурс памяти текущего кадра для std::pmr-контейнеров.
    std::pmr::memory_resource* resource() { return &curRes_; }

    usize totalBytes() const { return cur_.totalBytes() + prev_.totalBytes(); }

private:
    Arena cur_{4u << 20};
    Arena prev_{4u << 20};
    ArenaResource curRes_{cur_};
};

} // namespace mem
