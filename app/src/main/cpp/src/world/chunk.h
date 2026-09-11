#pragma once
#include "../core/types.h"
#include "block.h"
#include <array>
#include <atomic>
#include <glm/glm.hpp>
#include <vector>

namespace world {

constexpr i32 CHUNK_SIZE = 32;
constexpr i32 CHUNK_SIZE_Y = 128;                // высота мира
constexpr i32 CHUNK_VOL = CHUNK_SIZE * CHUNK_SIZE_Y * CHUNK_SIZE;
constexpr i32 CHUNK_MASK = CHUNK_SIZE - 1;

// Индексация внутри чанка: (y * CHUNK_SIZE + z) * CHUNK_SIZE + x
inline i32 chunkIndex(i32 x, i32 y, i32 z) {
    return (y * CHUNK_SIZE + z) * CHUNK_SIZE + x;
}

// Готовый квад после greedy meshing. Упаковка минимальная.
struct QuadVertex {
    glm::vec3 pos;
    u16 block;
    u8  face;    // 0..5
    u8  normal;  // запакованная нормаль (индекс)
};
struct Quad {
    QuadVertex v0;      // origin
    glm::vec3  du;      // вектор вдоль u
    glm::vec3  dv;      // вектор вдоль v
    // UV-координаты тайла в атласе
    glm::vec2  uv;
};

struct ChunkMesh {
    std::vector<Quad>  quads;
    std::atomic<bool>  ready{false};
    u64                revision = 0;
};

// LOD уровни: 0 = полный, 1 = 1/2, 2 = 1/4, 3 = 1/8 (по рёбрам)
enum class Lod : u8 { Full = 0, Half = 1, Quarter = 2, Eighth = 3 };

// ============================================================
// Данные одного чанка. Держит воксели + метаданные + 2 уровня меша (LOD0, LOD1).
// Границы обрабатываются через указатели на соседей на этапе mesh.
// ============================================================
struct Chunk {
    glm::ivec3 coord{0};
    std::array<u16, CHUNK_VOL> voxels{};   // block ids
    std::array<u8,  CHUNK_VOL> light{};    // 0..15 запёкшийся свет

    // в struct Chunk:
    std::array<ChunkMesh, 4> meshes;  // LOD 0..3

    // Флаги состояния асинхронного пайплайна
    std::atomic<bool> generated{false};
    std::atomic<bool> meshQueued{false};
    std::atomic<u64>  version{0};

    // GPU-хендлы (заполняются при upload, читаются рендером)
    u32 gpuVbLod0 = 0;
    u32 gpuIbLod0 = 0;
    u32 gpuVbLod1 = 0;
    u32 gpuIbLod1 = 0;
    u32 indexCountLod0 = 0;
    u32 indexCountLod1 = 0;

    // ---- Доступ к вокселям ----
    inline u16 at(i32 x, i32 y, i32 z) const {
        if ((u32)x >= (u32)CHUNK_SIZE || (u32)y >= (u32)CHUNK_SIZE_Y || (u32)z >= (u32)CHUNK_SIZE)
            return AIR;
        return voxels[chunkIndex(x, y, z)];
    }
    inline void set(i32 x, i32 y, i32 z, u16 id) {
        if ((u32)x >= (u32)CHUNK_SIZE || (u32)y >= (u32)CHUNK_SIZE_Y || (u32)z >= (u32)CHUNK_SIZE)
            return;
        voxels[chunkIndex(x, y, z)] = id;
        version.fetch_add(1, std::memory_order_relaxed);
    }
    inline bool inBounds(i32 x, i32 y, i32 z) const {
        return (u32)x < (u32)CHUNK_SIZE && (u32)y < (u32)CHUNK_SIZE_Y && (u32)z < (u32)CHUNK_SIZE;
    }
};

// ============================================================
// Greedy Meshing. Возвращает количество квадов.
// Соседи нужны для корректной отсечки граней на границах чанка.
// ============================================================
struct ChunkNeighbors {
    const Chunk* nx = nullptr;
    const Chunk* px = nullptr;
    const Chunk* ny = nullptr;
    const Chunk* py = nullptr;
    const Chunk* nz = nullptr;
    const Chunk* pz = nullptr;
};

u32 buildGreedyMesh(const Chunk& chunk, const ChunkNeighbors& nb,
                    std::vector<Quad>& outQuads, Lod lod);

} // namespace world