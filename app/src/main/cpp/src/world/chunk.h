/**
 * @file chunk.h
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#pragma once
#include "../core/types.h"
#include "block.h"
#include <array>
#include <atomic>
#include <glm/glm.hpp>
#include <mutex>
#include <shared_mutex>
#include <memory_resource>
#include <vector>

namespace world {

constexpr i32 CHUNK_SIZE = 32;
constexpr i32 CHUNK_SIZE_Y = 128;                // высота мира
constexpr i32 CHUNK_VOL = CHUNK_SIZE * CHUNK_SIZE_Y * CHUNK_SIZE;
constexpr i32 CHUNK_MASK = CHUNK_SIZE - 1;

/// Индексация внутри чанка: (y * CHUNK_SIZE + z) * CHUNK_SIZE + x
inline i32 chunkIndex(i32 x, i32 y, i32 z) {
    return (y * CHUNK_SIZE + z) * CHUNK_SIZE + x;
}

/// Готовый квад после greedy meshing. Упаковка минимальная.
struct QuadVertex {
    glm::vec3 pos;
    u16 block;
    u8  face;    // 0..5
    u8  normal;  // запакованная нормаль (индекс)
};
/// Слитый greedy-квад. Цвет выбирается на этапе построения вершин по
/// block и face, поэтому здесь его хранить не нужно.
struct Quad {
    QuadVertex v0;      ///< угол-начало
    glm::vec3  du;      ///< вектор вдоль оси u, длина = ширина в блоках
    glm::vec3  dv;      ///< вектор вдоль оси v, длина = высота в блоках
    /// Открытость неба в углах p0..p3, 0..7: 7 — над углом ничего
    /// нет, 0 — он глубоко под сводом. Без этого пещера освещена так
    /// же, как открытый склон, и мир читается как декорация без
    /// внутренностей.
    u8         sky[4];
    /// Затенение в углах p0..p3 (начало, +du, +du+dv, +dv), 0..3:
    /// 0 — угол зажат соседями, 3 — открыт. Без текстур это главное,
    /// что придаёт вокселям объём, поэтому оно запекается прямо в
    /// геометрию. Слияние граней учитывает его: квады с разным
    /// затенением не склеиваются, иначе тень «потечёт» по всей плоскости.
    u8         ao[4];

    // Packed render-only metadata for the top face of WATER:
    // bits 0..2 direction (8 compass directions),
    // bits 3..5 speed (0..7),
    // bits 6..7 local turbulence (0..3).
    u8         waterFlow = 0;
};

struct ChunkMesh {
    /// Квады живут только до выгрузки на GPU: сразу после неё список
    /// освобождается. При 48 байтах на квад и десяти тысячах квадов на
    /// чанк держать их постоянно — это сотни мегабайт на телефоне ради
    /// данных, которые уже лежат в видеопамяти. Если уровень
    /// понадобится снова, чанк перемешивается заново, в фоне.
    std::vector<Quad>  quads;
    std::atomic<bool>  ready{false};   ///< есть неотправленные на GPU данные
    u64                revision = 0;   ///< Chunk::version на момент построения
    bool               built = false;  ///< quads заполнены хотя бы раз
};

// Здесь был enum Lod и вместе с ним вся система уровней детализации:
// четыре меша на чанк, огрубление вокселей в клетки 2/4/8, юбки на
// стыках уровней, гистерезис у границ и машина состояний
// residentLod/targetLod/requestedLod в рендере. Всё это убрано: мир
// мешится и рисуется в полной детализации.

/// Данные одного чанка: воксели, метаданные и построенный меш.
/// Границы обрабатываются через указатели на соседей на этапе mesh.
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

    /// Высота поверхности по колонкам, индексация x * CHUNK_SIZE + z —
    /// та же, что у TerrainGenerator::Column.
    ///
    /// Ровно то число, которое вернул бы generator().surfaceHeight()
    /// для этой колонки: генерация всё равно считает его для каждой
    /// колонки чанка, и выбросить его, чтобы через кадр посчитать
    /// заново, — самая дорогая мелочь в проекте. Расчёт колонки — это
    /// все шумовые поля биома плюс три октавы рельефа, полмикросекунды
    /// на колонку. Миникарта звала его шестнадцать тысяч раз за
    /// проход (восемь миллисекунд), трава — шестьсот раз за
    /// пересборку, и оба раза для местности, которая уже лежит в
    /// памяти рядом.
    ///
    /// Две тысячи байт на чанк против четверти мегабайта вокселей.
    /// Пишется под voxelMutex до того, как чанк помечен generated.
    std::array<i16, CHUNK_SIZE * CHUNK_SIZE> surfaceY{};

    // ------------------------------------------------------------
    // Готовый меш чанка.
    // Пишет задача меширования, читает поток рендера — под meshMutex.
    // ------------------------------------------------------------
    mutable std::mutex       meshMutex;
    /// Меш чанка. Один: уровней детализации больше нет.
    ChunkMesh mesh;


    /// Состояние асинхронного конвейера.
    std::atomic<bool> generated{false};
    std::atomic<bool> removed{false};   ///< выгружен: задачи должны выйти
    /// Чанк уже стоит в очереди готовых мешей. Раньше дубликаты
    /// отсеивались линейным поиском по всей очереди под общим
    /// замком — из каждого рабочего потока, на каждый достроенный
    /// меш.
    std::atomic<bool> queuedForUpload{false};
    std::atomic<u64>  version{0};       ///< растёт при каждом изменении вокселей

    /// Номер заказа на меширование, растёт при каждой постановке задачи.
    ///
    /// Задача уносит номер с собой и перед записью сверяет его с
    /// текущим: если пока она считала, меш заказали заново, её
    /// результат — уже прошлое, и записывать его нельзя. Без этой
    /// сверки старая задача затирала состояние более нового запроса.
    std::atomic<u64>  meshSeq{0};

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

/// Greedy Meshing. Возвращает количество квадов.
/// Соседи нужны для корректной отсечки граней на границах чанка.
struct ChunkNeighbors {
    const Chunk* nx = nullptr;
    const Chunk* px = nullptr;
    const Chunk* ny = nullptr;
    const Chunk* py = nullptr;
    const Chunk* nz = nullptr;
    const Chunk* pz = nullptr;
};

/// Строит слитый меш чанка в полной детализации.
/// Контейнер вывода шаблонный: задача меширования складывает квады
/// в std::pmr::vector поверх арены воркера, тесты — в обычный vector.
template<typename QuadContainer>
u32 buildGreedyMeshInto(const Chunk& chunk, const ChunkNeighbors& nb,
                        QuadContainer& outQuads);

/// Чтение вокселя с учётом границ чанка.
///
/// Возвращает три разных исхода, а не два: настоящий блок, AIR — и
/// UNKNOWN, если соседнего чанка ещё нет. Мешер обязан их различать:
/// по неизвестной стороне грань не строится. Объявлено здесь, чтобы
/// договор можно было проверить, а не только прочитать.
u16 sampleVoxel(const Chunk& c, const ChunkNeighbors& nb, i32 x, i32 y, i32 z);

u32 buildGreedyMesh(const Chunk& chunk, const ChunkNeighbors& nb,
                    std::vector<Quad>& outQuads);
u32 buildGreedyMesh(const Chunk& chunk, const ChunkNeighbors& nb,
                    std::pmr::vector<Quad>& outQuads);

} // namespace world
