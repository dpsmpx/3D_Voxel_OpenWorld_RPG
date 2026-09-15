/**
 * @file mob_renderer.h
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
#include "../ecs/registry.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

namespace render {

/// Формат инстанса: pos (12) + size (12) + color (4) + _pad (4)
/// + кватернион (16) = 48. Одна коробка сущности.
///
/// Раньше здесь был один угол `yaw` на всю коробку — и этого хватало
/// ровно до тех пор, пока части не понадобилось ВРАЩАТЬ. Повёрнутую
/// конечность такой формат выразить не может, поэтому анимация и
/// сдвигала ногу параллельно себе вместо качания в суставе.
///
/// Теперь кватернион: 32 → 48 байт на коробку. При полусотне сущностей
/// по дюжине частей это 600 коробок, то есть 28 КБ вместо 19 — цена
/// пренебрежимая, а качание в суставе становится возможным.
struct MobInstance {
    glm::vec3 pos;      ///< центр коробки в мире
    glm::vec3 size;
    u32       color;
    f32       _pad;     ///< выравнивание кватерниона на 16 байт
    glm::vec4 rot;      ///< кватернион (x, y, z, w)
};
static_assert(sizeof(MobInstance) == 48);

/// ЕДИНСТВЕННОЕ объявление вершинного формата для MobInstance.
///
/// Раньше эту таблицу дословно повторяли четыре рендера — мобы,
/// NPC, предметы и снаряды, — и смещения в ней были набраны руками.
/// Стоило формату инстанса измениться, как правку получал один
/// рендер из четырёх, а остальные продолжали читать чужие байты:
/// Vulkan такое не сообщает, картинка просто становится мусором.
/// Поэтому таблица существует в одном экземпляре, а её смещения
/// сверяются с полями структуры static_assert'ами ниже.
static const vk::VertexBinding MOB_BINDINGS[2] = {
    { 12,                  false },   // единичный куб: glm::vec3
    { sizeof(MobInstance), true  },
};
static const vk::VertexAttr MOB_ATTRS[5] = {
    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  },   // inPos (единичный куб)
    { 1, 1, VK_FORMAT_R32G32B32_SFLOAT,    0  },   // iPos
    { 2, 1, VK_FORMAT_R32G32B32_SFLOAT,    12 },   // iSize
    { 3, 1, VK_FORMAT_R8G8B8A8_UNORM,      24 },   // iColor
    { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },   // iRot (кватернион)
};
// Числа выше обязаны совпадать с полями MobInstance. Проверяет это
// компилятор, а не внимательность: разъехавшиеся руками смещения и
// были исходной ошибкой.
static_assert(offsetof(MobInstance, pos)   ==  0);
static_assert(offsetof(MobInstance, size)  == 12);
static_assert(offsetof(MobInstance, color) == 24);
static_assert(offsetof(MobInstance, rot)   == 32);
constexpr u32 MOB_BINDING_COUNT = 2;
constexpr u32 MOB_ATTR_COUNT    = 5;

/// Единичный куб: 24 вершины, по четыре на грань, в порядке
/// -Z, +Z, -X, +X, -Y, +Y. Порядок граней знает mob.vert: он берёт
/// из него нормаль как gl_VertexIndex / 4.
struct CubeVertex { glm::vec3 pos; };
static_assert(sizeof(CubeVertex) == 12, "MOB_BINDINGS рассчитан на 12 байт");
extern const CubeVertex MOB_CUBE_V[24];
extern const u32        MOB_CUBE_I[36];

class MobRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    /// Перестраивает инстанс-буфер из ECS-сущностей
    /// timeSec — монотонное время кадра: на нём идёт дыхание
/// в покое. Фаза шага берётся из ecs::Gait, она идёт путём.
    void rebuild(ecs::Registry& reg, f32 timeSec);

    /// Загружает инстансы в GPU. Вызывается 1 раз за кадр.
    void upload(vk::Context& ctx);

    /// Рисует
    /// set передаётся явно и привязывается своим layout'ом. Раньше
    /// дескрипторы брались те, что оставил после себя рендер чанков:
    /// Vulkan гарантирует их сохранность только при совместимых
    /// layout'ах, а совместимость ломается от любого расхождения —
    /// например от push-константы, которой у чанков теперь есть.
    void render(vk::Context& ctx, VkDescriptorSet set, const math::Frustum& frustum);

    u32 instanceCount() const { return instanceCount_; }

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    vk::Buffer           vbo_;   // unit cube
    vk::Buffer           ibo_;   // 36 indices
    InstanceRing         instances_;

    std::vector<MobInstance> cpuInstances_;
    u32 instanceCount_ = 0;
};

} // namespace render
