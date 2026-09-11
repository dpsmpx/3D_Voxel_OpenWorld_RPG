#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include "../ecs/registry.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace render {

// Формат инстанса: pos (12) + size (12) + color (4) + yaw (4) = 32
struct MobInstance {
    glm::vec3 pos;
    glm::vec3 size;
    u32       color;
    f32       yaw;
};
static_assert(sizeof(MobInstance) == 32);

class MobRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    // Перестраивает инстанс-буфер из ECS-сущностей
    void rebuild(ecs::Registry& reg);

    // Загружает инстансы в GPU. Вызывается 1 раз за кадр.
    void upload(vk::Context& ctx);

    // Рисует
    void render(vk::Context& ctx, const math::Frustum& frustum);

    u32 instanceCount() const { return instanceCount_; }

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    vk::Buffer           vbo_;   // unit cube
    vk::Buffer           ibo_;   // 36 indices
    vk::Buffer           instanceGpu_;
    u64                  instanceCapacity_ = 0;

    std::vector<MobInstance> cpuInstances_;
    u32 instanceCount_ = 0;
};

} // namespace render