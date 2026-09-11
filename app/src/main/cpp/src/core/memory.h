#pragma once
#include "types.h"
#include "log.h"
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <memory>

namespace mem {

// ============================================================
// Arena — bump-аллокатор блоками по 1 МБ.
// Идеально для per-frame данных, временных буферов генерации.
// reset() не освобождает память, а переиспользует блоки.
// ============================================================
class Arena {
public:
    explicit Arena(usize blockSize = 1u << 20)
        : blockSize_(blockSize) {}

    ~Arena() { clear(); for (auto b : blocks_) ::free(b); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    
    // Разрешаем перемещение для работы std::swap
    Arena(Arena&&) noexcept = default;
    Arena& operator=(Arena&&) noexcept = default;

    void* alloc(usize size, usize align = alignof(std::max_align_t)) {
        ASSERT(size > 0);
        usize mask = align - 1;
        usize aligned = (offset_ + mask) & ~mask;
        if (aligned + size > blockSize_) {
            newBlock();
            aligned = (offset_ + mask) & ~mask;
        }
        void* p = (u8*)blocks_.back() + aligned;
        offset_ = aligned + size;
        return p;
    }

    template<typename T, typename... Args>
    T* make(Args&&... args) {
        void* p = alloc(sizeof(T), alignof(T));
        return new (p) T(std::forward<Args>(args)...);
    }

    // Сброс — переходим к первому блоку, offset = 0
    void reset() { current_ = 0; offset_ = 0; }

    usize totalBytes() const { return blocks_.size() * blockSize_; }

private:
    void newBlock() {
        ++current_;
        if (current_ < blocks_.size()) {
            offset_ = 0;
            return;
        }
        void* p = ::malloc(blockSize_);
        ASSERT_MSG(p, "Arena OOM");
        blocks_.push_back((u8*)p);
        offset_ = 0;
    }

    void clear() { current_ = 0; offset_ = 0; }

    usize blockSize_;
    std::vector<u8*> blocks_;
    usize current_ = 0;
    usize offset_  = 0;
};

// ============================================================
// Pool<T> — фиксированный пул объектов с free-list.
// Для сущностей, мобов, снарядов — слоты переиспользуются.
// ============================================================
template<typename T>
class Pool {
public:
    explicit Pool(usize capacity = 1024)
        : capacity_(capacity), data_(capacity), free_(capacity)
    {
        for (usize i = 0; i < capacity_; ++i) free_[i] = (u32)(capacity_ - 1 - i);
        freeTop_ = capacity_;
    }

    T* alloc() {
        if (freeTop_ == 0) grow();
        u32 idx = free_[--freeTop_];
        return new (&data_[idx]) T();
    }

    void free(T* p) {
        if (!p) return;
        usize idx = (usize)(p - data_.data());
        ASSERT(idx < capacity_);
        p->~T();
        free_[freeTop_++] = (u32)idx;
    }

    usize freeSlots() const { return freeTop_; }

private:
    void grow() {
        usize oldCap = capacity_;
        capacity_ *= 2;
        data_.resize(capacity_);
        free_.resize(capacity_);
        for (usize i = oldCap; i < capacity_; ++i) free_[freeTop_++] = (u32)(capacity_ - 1 - (i - oldCap));
    }

    usize capacity_;
    std::vector<T>     data_;
    std::vector<u32>   free_;
    usize              freeTop_ = 0;
};

// ============================================================
// FrameAllocator — двойной буфер: current/previous.
// Всё, что аллоцировано в этом кадре, живёт до конца следующего.
// ============================================================
class FrameAllocator {
public:
    void beginFrame() {
        prev_.reset();
        std::swap(cur_, prev_);
    }
    void* alloc(usize size, usize align = alignof(std::max_align_t)) {
        return cur_.alloc(size, align);
    }
    template<typename T, typename... Args>
    T* make(Args&&... args) { return cur_.make<T>(std::forward<Args>(args)...); }

private:
    Arena cur_{4u << 20};
    Arena prev_{4u << 20};
};

} // namespace mem
