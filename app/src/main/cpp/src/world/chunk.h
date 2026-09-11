#pragma once
#include "../core/types.h"
#include "block.h"
#include <array>
#include <atomic>
#include <glm/glm.hpp>
#include <mutex>
#include <shared_mutex>
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
/// Слитый greedy-квад. Тайл атласа выбирается на этапе построения
/// вершин по block и face, поэтому здесь его хранить не нужно.
struct Quad {
    QuadVertex v0;      ///< угол-начало
    glm::vec3  du;      ///< вектор вдоль оси u, длина = ширина в блоках
    glm::vec3  dv;      ///< вектор вдоль оси v, длина = высота в блоках
};

struct ChunkMesh {
    std::vector<Quad>  quads;
    std::atomic<bool>  ready{false};   ///< есть неотправленные на GPU данные
    u64                revision = 0;   ///< Chunk::version на момент построения
    bool               built = false;  ///< quads заполнены хотя бы раз
};

// LOD уровни: 0 = полный, 1 = 1/2, 2 = 1/4, 3 = 1/8 (по рёбрам)
enum class Lod : u8 { Full = 0, Half = 1, Quarter = 2, Eighth = 3 };

// ============================================================
// Данные одного чанка. Держит воксели + метаданные + 2 уровня меша (LOD0, LOD1).
// Границы обрабатываются через указатели на соседей на этапе mesh.
// ============================================================
struct Chunk {
    glm::ivec3 coord{0};

    // ------------------------------------------------------------
    // Воксельные данные. Пишет главный поток (setVoxel, загрузка
    // сейва) и задача генерации; читают задачи меширования и
    // физика. Любой доступ — под voxelMutex: unique для записи,
    // shared для чтения.
    // ------------------------------------------------------------
    mutable std::shared_mutex  voxelMutex;
    std::array<u16, CHUNK_VOL> voxels{};   ///< ID блоков
    std::array<u8,  CHUNK_VOL> light{};    ///< 0..15, запечённый свет

    // ------------------------------------------------------------
    // Готовые меши, по одному на уровень детализации.
    // Пишет задача меширования, читает поток рендера — под meshMutex.
    // ------------------------------------------------------------
    mutable std::mutex       meshMutex;
    std::array<ChunkMesh, 4> meshes;

    // Состояние асинхронного конвейера.
    std::atomic<bool> generated{false};
    std::atomic<bool> meshQueued{false};
    std::atomic<bool> removed{false};   ///< выгружен: задачи должны выйти
    std::atomic<u64>  version{0};       ///< растёт при каждом изменении вокселей

    // ---- Доступ к вокселям ----
    /// Чтение вокселя. Вызывающий должен удерживать voxelMutex
    /// (хотя бы shared) — как это делает меширование.
    inline u16 at(i32 x, i32 y, i32 z) const {
        if ((u32)x >= (u32)CHUNK_SIZE || (u32)y >= (u32)CHUNK_SIZE_Y || (u32)z >= (u32)CHUNK_SIZE)
            return AIR;
        return voxels[chunkIndex(x, y, z)];
    }
    /// Записывает воксель, сам захватывая voxelMutex.
    /// Не вызывать, если замок уже удерживается — используйте setUnlocked().
    inline void set(i32 x, i32 y, i32 z, u16 id) {
        if ((u32)x >= (u32)CHUNK_SIZE || (u32)y >= (u32)CHUNK_SIZE_Y || (u32)z >= (u32)CHUNK_SIZE)
            return;
        {
            std::unique_lock lk(voxelMutex);
            voxels[chunkIndex(x, y, z)] = id;
        }
        version.fetch_add(1, std::memory_order_release);
    }

    /// Запись без захвата замка: вызывающий уже держит voxelMutex
    /// уникально (генерация чанка, применение дельт сейва).
    inline void setUnlocked(i32 x, i32 y, i32 z, u16 id) {
        if ((u32)x >= (u32)CHUNK_SIZE || (u32)y >= (u32)CHUNK_SIZE_Y || (u32)z >= (u32)CHUNK_SIZE)
            return;
        voxels[chunkIndex(x, y, z)] = id;
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
