#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_context.h"
#include "../vk/vk_staging_pool.h"
#include "../world/chunk_manager.h"
#include "mesh_builder.h"
#include <unordered_map>
#include <vector>

namespace render {

class ChunkRenderer {
public:
    bool init(VkDevice dev, VkPhysicalDevice phys);
    void shutdown();

    void uploadChunks(vk::Context& ctx, const std::vector<world::Chunk*>& chunks);
    void forgetChunk(world::ChunkCoord c);

    void render(vk::Context& ctx, VkPipeline pipe, VkPipelineLayout layout,
                VkDescriptorSet set, const math::Frustum& frustum,
                const glm::vec3& cameraPos);

    // Метрики
    u32 lastDrawnChunks() const { return lastDrawnChunks_; }
    u32 lastDrawnIndices() const { return lastDrawnIndices_; }
    u32 lastLodCounts(int lod) const { return lodCounts_[lod]; }

private:
    struct GpuMesh {
        vk::Buffer vb;
        vk::Buffer ib;
        u32 indexCount = 0;
        bool valid = false;
    };
    struct ChunkGpu {
        GpuMesh lod[4];
    };

    // LOD thresholds (в метрах, кв.расстояние)
    static constexpr f32 LOD0_SQ = 64.f * 64.f;
    static constexpr f32 LOD1_SQ = 160.f * 160.f;
    static constexpr f32 LOD2_SQ = 320.f * 320.f;

    VkDevice         dev_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;

    vk::StagingPool staging_;

    std::unordered_map<world::ChunkCoord, ChunkGpu, world::ChunkCoordHash> meshes_;

    std::vector<VoxelVertex> scratchVerts_;
    std::vector<u32>         scratchIndices_;

    u32 lastDrawnChunks_  = 0;
    u32 lastDrawnIndices_ = 0;
    u32 lodCounts_[4]     = {0, 0, 0, 0};
};

} // namespace render