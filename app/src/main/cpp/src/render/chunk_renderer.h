/**
 * @file chunk_renderer.h
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_context.h"
#include "../vk/vk_staging_pool.h"
#include "../world/chunk_manager.h"
#include "mesh_builder.h"
#include "../world/lod.h"
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace render {

/// «Уровня нет» — и для резидентного, и для целевого.
constexpr u8 LOD_NONE = 0xFF;

/// Кому принадлежит residentLod после того, как на GPU приехал
/// уровень `arrived`. ЕДИНСТВЕННОЕ правило, по которому резидентный
/// уровень вообще меняется.
///
/// Отдельная свободная функция, а не три строки внутри uploadChunks,
/// потому что проверять надо именно её: порядок, в котором
/// заканчиваются задачи меширования, снаружи не воспроизвести, а
/// правило — воспроизводится целиком.
///
/// Ровно три случая:
///  * приехало то, что заказано, — оно и становится резидентным;
///  * рисовать пока нечем — годится что угодно, лучше грубая
///    геометрия, чем дыра в ландшафте;
///  * иначе приехало ЧУЖОЕ завершение (задача за другой уровень,
///    поставленная раньше и закончившаяся позже) — и оно не двигает
///    резидентный уровень никуда.
///
/// Последний случай и есть исправление: раньше резидентным
/// назначалось всё, что доехало, поэтому при неподвижной камере
/// разрешение дальнего рельефа скакало по LOD0..LOD3 в зависимости
/// от того, какой воркер закончил первым.
inline u8 residentAfterUpload(u8 resident, u8 target, u8 arrived) {
    if (arrived > 3)        return resident;   // мусор не принимаем
    if (arrived == target)  return arrived;    // ровно то, что заказано
    if (resident > 3)       return arrived;    // дыру закрыть важнее
    return resident;                           // чужое завершение ничего не двигает
}

class ChunkRenderer {
public:
    /// Сколько свежепостроенных мешей берём в один кадр. Каждый стоит
    /// сборки вершин на процессоре и копии в видеопамять; при загрузке
    /// мира их приезжает по нескольку десятков разом.
    static constexpr usize MAX_MESH_UPLOADS_PER_FRAME = 12;

    bool init(VkDevice dev, VkPhysicalDevice phys);
    void shutdown();

    /// Загружает на GPU меши только что перестроенных чанков и
    /// обслуживает отложенные запросы LOD от render(). Все копии
    /// собираются в один пакет передачи — одна остановка GPU на кадр.
    void uploadChunks(vk::Context& ctx, world::ChunkManager& world,
                      const std::vector<world::MeshReady>& ready,
                      const glm::vec3& cameraPos);

    void forgetChunk(world::ChunkCoord c);

    /// Дальность прорисовки в блоках. Задаёт границы LOD так, чтобы
    /// огрубление всегда приходилось на задымлённую даль.
    void setViewDistanceBlocks(f32 blocks) { bands_.fromViewDistance(blocks); }
    const world::LodBands& bands() const { return bands_; }

    /// Отбирает видимые чанки, раскладывает их по расстоянию и
    /// рисует в два прохода: непрозрачное от ближнего к дальнему,
    /// полупрозрачное — наоборот.
    ///
    /// Порядок здесь не косметика. Мобильные GPU отбрасывают
    /// закрытые фрагменты по глубине ДО фрагментного шейдера, но
    /// только если ближнее уже нарисовано: обход в порядке
    /// хэш-таблицы, как было раньше, отдавал эту экономию даром.
    /// А смешивание, наоборот, требует обратного порядка, иначе
    /// вода поверх воды складывается неправильно.
    void render(vk::Context& ctx,
                VkPipeline opaquePipe, VkPipeline blendPipe,
                VkPipelineLayout layout,
                VkDescriptorSet set, const math::Frustum& frustum,
                const glm::vec3& cameraPos);

    /// Границы уровней детализации в блоках — их же берёт мир,
    /// чтобы мешировать новый чанк сразу в нужном разрешении.
    f32 lodBand(int i) const { return bands_.bound((u8)i); }

    /// Метрики
    u32 lastDrawnChunks() const { return lastDrawnChunks_; }
    u32 lastDrawnIndices() const { return lastDrawnIndices_; }
    u32 lastLodCounts(int lod) const { return lodCounts_[lod]; }
    u32 lastEmptyChunks() const   { return lastEmptyChunks_; }
    u32 lastWaitingChunks() const { return lastWaitingChunks_; }

private:
    struct GpuMesh {
        vk::Buffer vb;
        vk::Buffer ib;
        /// Индексы непрозрачной части: [0, opaqueIndices).
        u32 opaqueIndices = 0;
        /// Всего индексов; хвост — полупрозрачные грани.
        u32 totalIndices  = 0;
        /// 16 бит, пока вершин меньше 65536 — а это почти всегда.
        /// Вдвое меньше индексного трафика на ровном месте.
        VkIndexType indexType = VK_INDEX_TYPE_UINT16;
        bool valid = false;
        /// Этот уровень уже побывал в видеопамяти. Отличает «чанк
        /// пуст — рисовать нечего» от «меш ещё не приехал»: в обоих
        /// случаях индексов ноль, но заказывать заново надо только
        /// во втором. Без этого различения пустое небо над головой
        /// заказывалось бы каждый кадр.
        bool uploaded = false;
        /// Chunk::version, по которой построен лежащий здесь меш.
        /// По ней видно, какие уровни устарели после правки блоков:
        /// завершение одного уровня само по себе больше не означает,
        /// что остальные протухли — их может строиться несколько
        /// сразу, и все по одним и тем же вокселям.
        u64 revision = 0;
        /// Центр полупрозрачной геометрии в координатах чанка.
        /// Сортировать проход со смешиванием по центру всего чанка
        /// нельзя: он на половине высоты мира, в шестидесяти блоках
        /// над прудом. См. mesh_builder.
        glm::vec3 blendCenter{ 0.f };
    };
    struct ChunkGpu {
        GpuMesh                        lod[4];
        std::shared_ptr<world::Chunk>  chunk;      ///< держим данные для догрузки LOD
        /// Уровень, который чанк ОБЯЗАН показывать: решение принимает
        /// рендер по расстоянию до камеры. Только совпадение с ним
        /// даёт приехавшему уровню право стать резидентным.
        u8                             targetLod   = LOD_NONE;
        /// Уровень, целиком лежащий в видеопамяти и рисуемый сейчас.
        /// Меняется ровно в одном месте — см. residentAfterUpload.
        u8                             residentLod = LOD_NONE;
        /// Какой уровень уже заказан у мира. Без этого рендер просит
        /// один и тот же уровень каждый кадр, пока задача считается,
        /// и очередь забивается дублями.
        u8                             requestedLod = LOD_NONE;
        /// И когда заказан. Отметка без срока годности превращала
        /// любую потерянную задачу в вечную дыру: заказ считался
        /// сделанным, меш не приезжал никогда, а повторить было
        /// некому. Задача теряется законно — например, если воксели
        /// изменились между постановкой и запуском, и задача вышла
        /// как устаревшая.
        u64                            requestedFrame = 0;
    };

    /// Отложенный запрос детализации: render() обнаружил, что нужного
    /// LOD нет в видеопамяти, uploadChunks() догрузит его в след. кадре.
    struct LodRequest { world::ChunkCoord coord; u8 lod; };

    /// Границы уровней детализации в блоках. Привязаны к дальности
    /// прорисовки: при малой дальности зашитые 64/160/320 огрубляли
    /// рельеф уже в двух шагах от игрока, при большой — наоборот,
    /// заставляли тащить полный меш туда, где его съедает туман.
    world::LodBands bands_;

    /// Сколько догрузок LOD обслуживаем за кадр — ограничивает пик
    /// нагрузки при быстром перемещении игрока.
    static constexpr u32 MAX_LOD_UPLOADS_PER_FRAME = 8;

    /// Через сколько показанных кадров повторить заказ уровня, если
    /// меш так и не приехал. Около двух секунд: достаточно редко,
    /// чтобы не забивать очередь, и достаточно часто, чтобы дыра не
    /// пережила поворот головы.
    static constexpr u64 LOD_REQUEST_RETRY = 120;

    // Формула выбора уровня живёт в world/lod.h и одна на весь
    // проект: и мир, и рендер зовут именно её.
    u8 lodForDistanceSq(f32 distSq) const {
        return world::lodForDistanceSq(distSq, bands_);
    }
    u8 lodForDistanceSq(f32 distSq, u8 resident) const {
        return world::lodForDistanceSq(distSq, bands_, resident);
    }

    /// Откладывает уничтожение буфера. Кадр, в котором из него ещё
    /// читает GPU, может быть в работе — освобождать сразу нельзя.
    void retire(vk::Buffer& b);
    /// Освобождает то, что GPU точно дочитал.
    void collectRetired();

    /// Загружает один уровень детализации чанка в уже открытый пакет.
    bool uploadLod(vk::Context& ctx, VkCommandBuffer cmd,
                   ChunkGpu& gpu, world::Chunk& chunk, u8 lod);

    VkDevice         dev_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;

    vk::StagingPool staging_;

    std::unordered_map<world::ChunkCoord, ChunkGpu, world::ChunkCoordHash> meshes_;

    /// Что рисуем в этом кадре, уже отобранное и отсортированное.
    struct Visible {
        GpuMesh* mesh;
        glm::vec3 origin;
        f32 distSq;
    };

    /// Полупрозрачная часть видимых чанков, со своим расстоянием —
    /// от камеры до самой воды, а не до центра чанка.
    struct Blended {
        GpuMesh* mesh;
        glm::vec3 origin;
        f32 distSq;
    };

    std::vector<Blended>      blended_;
    std::vector<VoxelVertex>  scratchVerts_;
    std::vector<u32>          scratchIndices_;
    std::vector<u16>          scratchIndices16_;
    std::vector<world::Quad>  scratchQuads_;
    std::vector<LodRequest>   lodRequests_;
    /// Из них те, чей меш уже построен и правда грузится.
    std::vector<LodRequest>   servable_;
    std::vector<Visible>      visible_;

    /// Буферы, отправленные на покой: номер кадра и дескрипторы.
    struct Retired { u64 frame; vk::Buffer::Handles h; };
    std::vector<Retired> retired_;
    /// Номер последнего показанного кадра — отсчёт для Retired.
    u64 frameNo_ = 0;
    /// Staging-буферы текущего пакета: освобождаются только после
    /// того, как GPU дочитал их (endTransferBatch).
    std::vector<vk::StagingBuffer*> pendingStaging_;

    u32 lastDrawnChunks_  = 0;
    u32 lastDrawnIndices_ = 0;
    u32 lodCounts_[4]     = {0, 0, 0, 0};
    /// Чанки, попавшие в пирамиду видимости, но не нарисованные: ни на
    /// одном уровне нет геометрии. Ровно это и видно на экране как
    /// дыра в ландшафте, и по картинке «дыра» неотличима от «за этим
    /// чанком просто нет мира». По числу — отличима.
    u32 lastEmptyChunks_  = 0;
    /// Из них те, чей меш ещё заказан и ожидается.
    u32 lastWaitingChunks_ = 0;
};

} // namespace render
