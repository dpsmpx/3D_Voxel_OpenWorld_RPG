#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include "../world/chunk_manager.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace render {

// Instance layout должен совпадать с vertex input pipeline.
struct GrassInstance {
    glm::vec3 pos;       // offset 0
    f32       scale;     // offset 12
    glm::vec2 uvOrigin;  // offset 16
    u8        r, g, b, a;// offset 24 (packed color)
    f32       yaw;       // offset 28
};
static_assert(sizeof(GrassInstance) == 32, "GrassInstance должен быть 32 байта");

// ============================================================
// InstancedRenderer — рисует cross-quad геометрию (биллборд),
// один draw-call на все instances.
//
// Инстансы заполняются методом populateGrass() из ChunkManager'а
// (логика спавна травы эволюционирует в Phase 7 под полноценную
// систему частиц/декораций).
// ============================================================
class InstancedRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    void populateGrass(const world::ChunkManager& world, const glm::vec3& playerPos, f32 radius);

    void upload(vk::Context& ctx);
    void render(vk::Context& ctx, const math::Frustum& frustum);

    u32 instanceCount() const { return instanceCount_; }

private:
    VkDevice              dev_ = VK_NULL_HANDLE;
    vk::ShaderCache       shaders_;
    vk::GraphicsPipeline  pipeline_;
    vk::Buffer            vbo_;
    vk::Buffer            ibo_;
    vk::Buffer            instanceGpu_;
    u64                   instanceCapacityBytes_ = 0;

    std::vector<GrassInstance> cpuInstances_;
    u32                   instanceCount_ = 0;
    u32                   indexCount_    = 0;
};

} // namespace render