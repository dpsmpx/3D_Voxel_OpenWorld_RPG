#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using f32 = float;
using f64 = double;
using usize = size_t;

// Генерационный хендл: index + generation.
// Защищает от использования "мёртвых" сущностей.
struct Entity {
    u32 id = 0;
    u32 gen = 0;

    bool valid() const { return id != 0; }
    operator u32() const { return id; }
    bool operator==(Entity o) const { return id == o.id && gen == o.gen; }
    bool operator!=(Entity o) const { return !(*this == o); }
};

namespace std {
    template<> struct hash<Entity> {
        size_t operator()(Entity e) const {
            return (size_t)e.id * 0x9E3779B97F4A7C15ULL ^ (size_t)e.gen;
        }
    };
}