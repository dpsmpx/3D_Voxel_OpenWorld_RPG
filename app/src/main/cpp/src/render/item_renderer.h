/**
 * @file item_renderer.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_buffer.h"
#include "instance_ring.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include "../ecs/registry.h"
#include "mob_renderer.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace render {

class ItemRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    void rebuild(ecs::Registry& reg);
    void upload(vk::Context& ctx);
    /// set передаётся явно и привязывается своим layout'ом. Раньше
    /// дескрипторы брались те, что оставил после себя рендер чанков:
    /// Vulkan гарантирует их сохранность только при совместимых
    /// layout'ах, а совместимость ломается от любого расхождения —
    /// например от push-константы, которой у чанков теперь есть.
    void render(vk::Context& ctx, VkDescriptorSet set);

    u32 instanceCount() const { return instanceCount_; }

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    vk::Buffer           vbo_;
    vk::Buffer           ibo_;
    InstanceRing         instances_;

    std::vector<MobInstance> cpu_;
    u32 instanceCount_ = 0;
};

} // namespace render
