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
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace render {

class ChunkRenderer {
public:
    bool init(VkDevice dev, VkPhysicalDevice phys);
    void shutdown();

    /// Загружает на GPU меши только что перестроенных чанков и
    /// обслуживает отложенные запросы LOD от render(). Все копии
    /// собираются в один пакет передачи — одна остановка GPU на кадр.
    void uploadChunks(vk::Context& ctx, world::ChunkManager& world,
                      const std::vector<std::shared_ptr<world::Chunk>>& chunks,
                      const glm::vec3& cameraPos);

    void forgetChunk(world::ChunkCoord c);

    /// Дальность прорисовки в блоках. Задаёт границы LOD так, чтобы
    /// огрубление всегда приходилось на задымлённую даль.
    void setViewDistanceBlocks(f32 blocks) {
        lod0_ = blocks * 0.35f < 64.f ? 64.f : blocks * 0.35f;
        lod1_ = blocks * 0.62f < 140.f ? 140.f : blocks * 0.62f;
        lod2_ = blocks * 0.90f;
        if (lod1_ < lod0_ * 1.2f) lod1_ = lod0_ * 1.2f;
        if (lod2_ < lod1_ * 1.2f) lod2_ = lod1_ * 1.2f;
    }

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
    f32 lodBand(int i) const { return bound((u8)i); }

    /// Метрики
    u32 lastDrawnChunks() const { return lastDrawnChunks_; }
    u32 lastDrawnIndices() const { return lastDrawnIndices_; }
    u32 lastLodCounts(int lod) const { return lodCounts_[lod]; }

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
    };
    struct ChunkGpu {
        GpuMesh                        lod[4];
        std::shared_ptr<world::Chunk>  chunk;      ///< держим данные для догрузки LOD
        u8                             residentLod = 0xFF;
        /// Какой уровень уже заказан у мира. Без этого рендер просит
        /// один и тот же уровень каждый кадр, пока задача считается,
        /// и очередь забивается дублями.
        u8                             requestedLod = 0xFF;
    };

    /// Отложенный запрос детализации: render() обнаружил, что нужного
    /// LOD нет в видеопамяти, uploadChunks() догрузит его в след. кадре.
    struct LodRequest { world::ChunkCoord coord; u8 lod; };

    /// Границы уровней детализации в блоках. Привязаны к дальности
    /// прорисовки: при малой дальности зашитые 64/160/320 огрубляли
    /// рельеф уже в двух шагах от игрока, при большой — наоборот,
    /// заставляли тащить полный меш туда, где его съедает туман.
    f32 lod0_ = 64.f, lod1_ = 160.f, lod2_ = 320.f;

    /// Мёртвая зона у границы: пока чанк не отошёл от неё на восьмую
    /// часть, уровень не меняется. Без неё шаг вперёд-назад на самой
    /// границе перестраивает меш каждый кадр, и рельеф мерцает.
    static constexpr f32 LOD_HYSTERESIS = 0.125f;

    /// Сколько догрузок LOD обслуживаем за кадр — ограничивает пик
    /// нагрузки при быстром перемещении игрока.
    static constexpr u32 MAX_LOD_UPLOADS_PER_FRAME = 8;

    u8 lodForDistanceSq(f32 distSq) const {
        if (distSq < lod0_ * lod0_) return 0;
        if (distSq < lod1_ * lod1_) return 1;
        if (distSq < lod2_ * lod2_) return 2;
        return 3;
    }

    /// То же, но с мёртвой зоной вокруг текущего уровня.
    u8 lodForDistanceSq(f32 distSq, u8 resident) const {
        const u8 want = lodForDistanceSq(distSq);
        if (want == resident || resident > 3) return want;
        const f32 edge = (want > resident) ? bound(resident) : bound(want);
        const f32 lo = edge * (1.f - LOD_HYSTERESIS);
        const f32 hi = edge * (1.f + LOD_HYSTERESIS);
        const f32 d = std::sqrt(distSq);
        return (d > lo && d < hi) ? resident : want;
    }

    f32 bound(u8 lod) const {
        return lod == 0 ? lod0_ : (lod == 1 ? lod1_ : lod2_);
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

    std::vector<VoxelVertex>  scratchVerts_;
    std::vector<u32>          scratchIndices_;
    std::vector<u16>          scratchIndices16_;
    std::vector<world::Quad>  scratchQuads_;
    std::vector<LodRequest>   lodRequests_;
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
};

} // namespace render
