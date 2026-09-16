/**
 * @file npc_renderer.h
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

class NpcRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    /// timeSec — монотонное время кадра: на нём идёт дыхание
/// в покое. Фаза шага берётся из ecs::Gait, она идёт путём.
    /// showPlayer — рисовать ли игрока. От первого лица его модель
    /// закрывала бы обзор собственной грудью.
    void rebuild(ecs::Registry& reg, f32 timeSec, bool showPlayer);
    void upload(vk::Context& ctx);
    /// set передаётся явно и привязывается своим layout'ом. Раньше
    /// дескрипторы брались те, что оставил после себя рендер чанков:
    /// Vulkan гарантирует их сохранность только при совместимых
    /// layout'ах, а совместимость ломается от любого расхождения —
    /// например от push-константы, которой у чанков теперь есть.
    void render(vk::Context& ctx, VkDescriptorSet set);

    u32 instanceCount() const { return instanceCount_; }

private:
    /// Игрок — такой же двуногий; своего конвейера ему не нужно.
    void appendPlayer(ecs::Registry& reg, f32 timeSec);

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
