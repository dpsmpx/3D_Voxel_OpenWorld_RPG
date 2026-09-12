/**
 * @file skybox.h
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include <android/asset_manager.h>

namespace render {

/// Skybox — fullscreen-triangle, рисуется ПЕРЕД сценой.
/// Depth test/write выключены, поверхность заполняет весь экран,
/// затем воксели перекрывают её.
class Skybox {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();
    /// set привязывается своим layout'ом. Небо рисуется первым в
    /// кадре, и до этой правки оно не привязывало дескрипторы вовсе:
    /// шейдер читал матрицы из набора, оставшегося от интерфейса
    /// прошлого кадра, с несовместимым layout'ом.
    void render(vk::Context& ctx, VkDescriptorSet set);

private:
    VkDevice              dev_ = VK_NULL_HANDLE;
    vk::ShaderCache       shaders_;
    vk::GraphicsPipeline  pipeline_;
};

} // namespace render
