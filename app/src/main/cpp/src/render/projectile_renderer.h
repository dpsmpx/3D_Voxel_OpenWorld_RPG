/**
 * @file projectile_renderer.h
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
#include "../world/precipitation.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace render {

/// Превратить капли в инстансы куба.
///
/// Отдельно от самого рендера, потому что проверять надо именно это:
/// капля обязана лететь ВДОЛЬ своей скорости (иначе дождь идёт
/// кубиками), снежинка — быть крупной и белой, а мёртвых частиц в
/// списке быть не должно вовсе.
void precipInstances(const world::Precipitation& p, f32 snowMix,
                     std::vector<MobInstance>& out);

class ProjectileRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    void rebuild(ecs::Registry& reg);

    /// Осадки: те же кубы, тот же формат инстанса, то же смешивание.
    ///
    /// Рисуются этим же конвейером намеренно. Свой конвейер ради них
    /// означал бы вторую копию того же куба, того же буфера вершин и
    /// тех же двух шейдеров — ради разницы в том, откуда берётся
    /// список инстансов.
    ///
    /// Зовётся ДО rebuild(): тот собирает кадр целиком.
    void setPrecip(const MobInstance* data, u32 count);

    u32 precipCount() const { return precipCount_; }
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
    std::vector<MobInstance> precip_;
    u32 instanceCount_ = 0;
    u32 precipCount_ = 0;
};

} // namespace render
