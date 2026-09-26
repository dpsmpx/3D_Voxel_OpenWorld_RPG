/**
 * @file voxel_model_renderer.h
 * @brief Рендер: воксельные модели из мелких вокселей, по экземпляру на вещь.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_context.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "instance_ring.h"
#include "mesh_table.h"
#include "voxel_model.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <cstddef>
#include <utility>
#include <vector>

namespace render {

/// Экземпляр модели: где стоит, как повёрнута, во сколько раз
/// увеличена и чем подкрашена. 48 байт.
struct VoxelModelInstance {
    glm::vec3 pos;      ///< куда встаёт начало модели (низ, середина)
    f32       scale;
    glm::vec4 rot;      ///< кватернион (x, y, z, w)
    /// Множитель цвета — байты R, G, B, A, как их читает видеокарта.
    /// Присваивать через packInstanceColor().
    u32       tintGpu;
    f32       _pad[3];
};
static_assert(sizeof(VoxelModelInstance) == 48);

/// Вершинный формат моделей: меш (привязка 0) и экземпляр (1).
/// Смещения набраны числами — и сверены с полями static_assert'ами
/// ниже: разъехавшиеся руками смещения и были исходной ошибкой у
/// рендеров MobInstance.
static const vk::VertexBinding VOXMODEL_BINDINGS[2] = {
    { sizeof(VoxelModelVertex),   false },
    { sizeof(VoxelModelInstance), true  },
};
static const vk::VertexAttr VOXMODEL_ATTRS[7] = {
    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  },   // inPos
    { 1, 0, VK_FORMAT_R8G8B8A8_UNORM,      12 },   // inColor
    { 2, 0, VK_FORMAT_R32G32B32_SFLOAT,    16 },   // inNormal
    { 3, 1, VK_FORMAT_R32G32B32_SFLOAT,    0  },   // iPos
    { 4, 1, VK_FORMAT_R32_SFLOAT,          12 },   // iScale
    { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },   // iRot
    { 6, 1, VK_FORMAT_R8G8B8A8_UNORM,      32 },   // iTint
};
static_assert(offsetof(VoxelModelVertex, pos)      ==  0);
static_assert(offsetof(VoxelModelVertex, colorGpu) == 12);
static_assert(offsetof(VoxelModelVertex, normal)   == 16);
static_assert(offsetof(VoxelModelInstance, pos)     ==  0);
static_assert(offsetof(VoxelModelInstance, scale)   == 12);
static_assert(offsetof(VoxelModelInstance, rot)     == 16);
static_assert(offsetof(VoxelModelInstance, tintGpu) == 32);
constexpr u32 VOXMODEL_BINDING_COUNT = 2;
constexpr u32 VOXMODEL_ATTR_COUNT    = 7;

/// Рисует сложные воксельные модели — сотни мелких вокселей каждая.
///
/// Кубом-на-воксель такую модель рисовать нельзя: меч — полторы
/// сотни вокселей, склянка — две, и десяток предметов на земле
/// превратился бы в тысячи коробок. Здесь у каждой модели готовый
/// меш — только видимые грани, соседние одного цвета слиты, — и все
/// меши лежат в ОДНОМ буфере. Экземпляры сортируются по модели, и
/// каждая модель рисуется одним вызовом на все свои экземпляры.
class VoxelModelRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    /// Загрузить меши. Номер модели — индекс в массиве.
    bool setModels(vk::Context& ctx, const std::vector<VoxelMesh>& meshes) {
        return meshes_.set(ctx, meshes);
    }
    u32  modelCount() const { return meshes_.count(); }

    /// Экземпляры кадра.
    void begin() { pending_.clear(); }
    void add(u32 model, const glm::vec3& pos, const glm::vec4& rot, f32 scale, u32 tintRgba);

    void upload(vk::Context& ctx);
    void render(vk::Context& ctx, VkDescriptorSet set);

    u32 instanceCount() const { return instanceCount_; }
    u32 drawCount() const { return (u32)draws_.size(); }

    /// Разложить экземпляры по моделям: одна запись — один вызов
    /// отрисовки. Отдельно от upload, чтобы проверять без GPU.
    struct Draw { u32 model = 0, firstInstance = 0, count = 0; };
    static void batch(std::vector<std::pair<u32, VoxelModelInstance>>& pending,
                      std::vector<VoxelModelInstance>& sorted,
                      std::vector<Draw>& draws);

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    MeshTable            meshes_;
    InstanceRing         instances_;

    std::vector<std::pair<u32, VoxelModelInstance>> pending_;
    std::vector<VoxelModelInstance> sorted_;
    std::vector<Draw> draws_;
    u32 instanceCount_ = 0;
};

} // namespace render
