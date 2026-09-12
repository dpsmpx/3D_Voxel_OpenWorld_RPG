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
#include "occlusion.h"
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
    void uploadChunks(vk::Context& ctx,
                      const std::vector<std::shared_ptr<world::Chunk>>& chunks,
                      const glm::vec3& cameraPos);

    void forgetChunk(world::ChunkCoord c);

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
    ///
    /// occlusion может быть nullptr — тогда работает только
    /// отсечение по пирамиде видимости.
    void render(vk::Context& ctx,
                VkPipeline opaquePipe, VkPipeline blendPipe,
                VkPipelineLayout layout,
                VkDescriptorSet set, const math::Frustum& frustum,
                const glm::vec3& cameraPos,
                OcclusionCuller* occlusion = nullptr);

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
    };

    /// Отложенный запрос детализации: render() обнаружил, что нужного
    /// LOD нет в видеопамяти, uploadChunks() догрузит его в след. кадре.
    struct LodRequest { world::ChunkCoord coord; u8 lod; };

    /// LOD thresholds (в метрах, кв.расстояние)
    static constexpr f32 LOD0_SQ = 64.f * 64.f;
    static constexpr f32 LOD1_SQ = 160.f * 160.f;
    static constexpr f32 LOD2_SQ = 320.f * 320.f;

    /// Сколько догрузок LOD обслуживаем за кадр — ограничивает пик
    /// нагрузки при быстром перемещении игрока.
    static constexpr u32 MAX_LOD_UPLOADS_PER_FRAME = 8;

    static u8 lodForDistanceSq(f32 distSq) {
        if (distSq < LOD0_SQ) return 0;
        if (distSq < LOD1_SQ) return 1;
        if (distSq < LOD2_SQ) return 2;
        return 3;
    }

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
    /// Staging-буферы текущего пакета: освобождаются только после
    /// того, как GPU дочитал их (endTransferBatch).
    std::vector<vk::StagingBuffer*> pendingStaging_;

    u32 lastDrawnChunks_  = 0;
    u32 lastDrawnIndices_ = 0;
    u32 lodCounts_[4]     = {0, 0, 0, 0};
};

} // namespace render
