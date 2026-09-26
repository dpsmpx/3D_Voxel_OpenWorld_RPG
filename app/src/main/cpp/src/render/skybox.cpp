/**
 * @file skybox.cpp
 * @brief Рендер: небо — полноэкранный проход после непрозрачного.
 */
#include "skybox.h"
#include "../core/log.h"

namespace render {

bool Skybox::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    shaders_.init(dev_, mgr);

    vk::PipelineDesc d{};
    d.renderPass   = ctx.renderPass();
    d.descLayout   = descLayout;
    d.vertName     = "shaders/sky.vert.spv";
    d.fragName     = "shaders/sky.frag.spv";
    d.depthFormat  = ctx.depthFormat();
    d.cullMode     = VK_CULL_MODE_NONE;
    // Проверка глубины ВКЛЮЧЕНА, запись — нет.
    //
    // Небо рисуется последним из непрозрачного, а его вершинный
    // шейдер кладёт z = 1.0, то есть на дальнюю плоскость. При
    // сравнении LESS_OR_EQUAL оно проходит только там, где глубина
    // осталась очищенной — то есть где ландшафта нет. Везде, где
    // ландшафт уже написал свою глубину, фрагмент отбрасывается ДО
    // фрагментного шейдера.
    //
    // Пока небо рисовалось первым и без проверки, его шейдер считался
    // для каждого пикселя экрана, включая те две трети, которые потом
    // закрывал ландшафт. Шейдер этот не дешёвый: тогда — семь
    // возведений в степень на светила (замер tools/gpubench на кадре
    // 2306x1080: 8.08 мс против 0.78 мс у ровной заливки), теперь —
    // хэши клеток облаков и звёзд (docs/ARCHITECTURE.md, «Небо»).
    // Довод тот же: закрытое ландшафтом не считать.
    d.depthTest    = true;
    d.depthWrite   = false;
    d.blend        = false;
    d.bindings     = nullptr;   // полноэкранный треугольник строится в шейдере
    d.bindingCount = 0;
    d.attrs        = nullptr;
    d.attrCount    = 0;

    if (!pipeline_.create(dev_, shaders_, d)) return false;
    LOGI("Skybox готов");
    return true;
}

void Skybox::render(vk::Context& ctx, VkDescriptorSet set) {
    if (!pipeline_.valid()) return;
    VkCommandBuffer cmd = ctx.currentCmd();

    ctx.setFullViewport(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);   // полноэкранный треугольник
}

void Skybox::destroy() {
    pipeline_.destroy();
    shaders_.destroyAll();
}

} // namespace render
