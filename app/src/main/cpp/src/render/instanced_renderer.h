/**
 * @file instanced_renderer.h
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../core/math.h"
#include "../vk/vk_buffer.h"
#include "instance_ring.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include "../world/chunk_manager.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace render {

/// Instance layout должен совпадать с vertex input pipeline.
///
/// Координат в атласе тут больше нет: трава, как и весь мир, рисуется
/// цветом материала, а не текстурой. Форму даёт сама геометрия —
/// сужающаяся кверху трапеция, — а оттенок приходит инстансом, чтобы
/// поляна не выглядела покрашенной одной банкой.
/// Какую долю радиуса занимает плавный уход травы. Пучок у самой
/// границы должен исчезать, а не мерцать точкой в один пиксель.
constexpr f32 GRASS_FADE = 0.45f;

/// Ниже какого экранного размера пучок вообще не отправляется на GPU.
///
/// Непрозрачная геометрия меньше пикселя ничем не сглаживается: она
/// либо закрашивает пиксель целиком, либо исчезает, и от кадра к кадру
/// перещёлкивает. Поле такой травы читается как россыпь мерцающих
/// точек, а не как трава. Три пикселя — это уже различимый штрих, и
/// ниже него пучок не нужен ни для чего.
constexpr f32 GRASS_MIN_PIXELS = 3.0f;

struct GrassInstance {
    glm::vec3 pos;       // offset 0
    f32       scale;     // offset 12
    u8        r, g, b, a;// offset 16 (packed color)
    f32       yaw;       // offset 20
};
static_assert(sizeof(GrassInstance) == 24, "GrassInstance должен быть 24 байта");

/// InstancedRenderer — рисует cross-quad геометрию (биллборд),
/// один draw-call на все instances.
///
/// Инстансы заполняются методом populateGrass() из ChunkManager'а
/// (логика спавна травы эволюционирует в Phase 7 под полноценную
/// систему частиц/декораций).
class InstancedRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    /// pixelsPerUnit — во сколько экранных пикселей превращается одна
    /// мировая единица на расстоянии в одну единицу от камеры:
    /// (высота кадра / 2) / tan(fov / 2). По ней считается настоящий
    /// экранный размер пучка, и всё, что мельче GRASS_MIN_PIXELS, на
    /// GPU не уезжает вовсе.
    void populateGrass(const world::ChunkManager& world, const glm::vec3& playerPos,
                       f32 radius, f32 pixelsPerUnit);

    void upload(vk::Context& ctx);
    /// set передаётся явно и привязывается своим layout'ом. Раньше
    /// дескрипторы брались те, что оставил после себя рендер чанков:
    /// Vulkan гарантирует их сохранность только при совместимых
    /// layout'ах, а совместимость ломается от любого расхождения —
    /// например от push-константы, которой у чанков теперь есть.
    void render(vk::Context& ctx, VkDescriptorSet set, const math::Frustum& frustum);

    u32 instanceCount() const { return instanceCount_; }

private:
    VkDevice              dev_ = VK_NULL_HANDLE;
    vk::ShaderCache       shaders_;
    vk::GraphicsPipeline  pipeline_;
    vk::Buffer            vbo_;
    vk::Buffer            ibo_;
    InstanceRing         instances_;

    std::vector<GrassInstance> cpuInstances_;
    u32                   instanceCount_ = 0;
    u32                   indexCount_    = 0;
};

} // namespace render
