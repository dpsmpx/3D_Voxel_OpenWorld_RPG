/**
 * @file skybox.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include <android/asset_manager.h>

namespace render {

/// Skybox — полноэкранный треугольник на дальней плоскости,
/// рисуется ПОСЛЕ всей непрозрачной геометрии и ДО полупрозрачной.
///
/// Проверка глубины включена, запись выключена: небо ложится только
/// туда, где ландшафта нет, а закрытые им пиксели отбрасываются до
/// фрагментного шейдера. Раньше небо шло первым и без проверки
/// глубины — тогда его шейдер считался для всего экрана, а ландшафт
/// закрашивал две трети посчитанного.
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
