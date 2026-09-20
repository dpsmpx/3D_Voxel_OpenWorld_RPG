/**
 * @file render_system.cpp
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#include "render_system.h"
#include "../world/debug_scene.h"
#include "voxel_pipeline.h"
#include "../core/log.h"
#include "../config/settings.h"
#include <glm/glm.hpp>

namespace render {

bool RenderSystem::init(vk::Context& ctx, AAssetManager* mgr) {
    dev_ = ctx.device();
    shaders_.init(dev_, mgr);

    if (!descriptors_.create(ctx.device(), vk::Context::MAX_FRAMES)) {
        LOGE("DescriptorSet не создан");
        return false;
    }

    for (u32 i = 0; i < vk::Context::MAX_FRAMES; ++i) {
        if (!uboBuffers_[i].create(ctx.device(), ctx.physicalDevice(),
                                   sizeof(CameraUbo),
                                   vk::BufferUsage::Uniform, true)) return false;
        descriptors_.bindUbo(i, uboBuffers_[i].handle(), sizeof(CameraUbo));
    }

    {
        vk::PipelineDesc d = voxelPipelineDesc(ctx.renderPass(), descriptors_.layout(),
                                               ctx.depthFormat());
        if (!voxelPipeline_.create(dev_, shaders_, d)) return false;
        makeVoxelBlendDesc(d);
        if (!voxelBlendPipeline_.create(dev_, shaders_, d)) return false;
    }

    if (!chunkRenderer_.init(dev_, ctx.physicalDevice())) return false;
    if (!skybox_.init(ctx, mgr, descriptors_.layout())) {
        LOGW("Skybox не инициализирован");
    }
    if (!blockOutline_.init(ctx, mgr, descriptors_.layout())) {
        LOGW("BlockOutline не инициализирован");
    }
    if (!mobRenderer_.init(ctx, mgr, descriptors_.layout())) {
        LOGW("MobRenderer не инициализирован");
    }
    if (!projRenderer_.init(ctx, mgr, descriptors_.layout())) {
        LOGW("ProjectileRenderer не инициализирован");
    }
    if (!npcRenderer_.init(ctx, mgr, descriptors_.layout())) {
        LOGW("NpcRenderer не инициализирован");
    }
    if (!itemRenderer_.init(ctx, mgr, descriptors_.layout())) {
        LOGW("ItemRenderer не инициализирован");
    }

    LOGI("RenderSystem готов (Phase 13)");
    return true;
}

void RenderSystem::prepareFrame(vk::Context& ctx,
                                world::ChunkManager& world,
                                ecs::Registry& registry,
                                f32 timeSec, f32 dt,
                                player::Player* player,
                                const physics::RayHit& targetHit,
                                f32 fps)
{
    // В минимальной сцене время стоит: иначе трава качается по
    // fogParams.w и два кадра не совпадут никогда.
    timeSec_       = config::settingsConst().debugScene
                   ? world::SCENE_TIME_SEC : timeSec;
    currentPlayer_ = player;
    currentWorld_  = &world;
    currentFps_    = fps;
    lastHit_       = targetHit;

    const auto ready = world.pollMeshesReady(render::ChunkRenderer::MAX_MESH_UPLOADS_PER_FRAME);
    chunkRenderer_.uploadChunks(ctx, world, ready);

    mobRenderer_.rebuild(registry, timeSec_);
    mobRenderer_.upload(ctx);

    // Модель игрока идёт в тот же поток инстансов, что и NPC: один
    // конвейер, один буфер, один вызов отрисовки на всех двуногих.
    npcRenderer_.rebuild(registry, timeSec_,
                         player && player->cameraMode ==
                             player::CameraMode::ThirdPerson);
    npcRenderer_.upload(ctx);

    projRenderer_.rebuild(registry);
    projRenderer_.upload(ctx);

    itemRenderer_.rebuild(registry);
    itemRenderer_.upload(ctx);

    const glm::vec3 camPos = camera_.position();

    // Применяем настройки viewDistance к миру
    const i32 vd = config::settingsConst().viewDistance;
    world.setViewDistance(vd);

    // Туман привязан к дальности прорисовки, а не к постоянным числам.
    // Иначе он кончается там, где мир ещё есть (или наоборот), и край
    // загруженных чанков виден обрывом на фоне чистого неба.
    const f32 vdBlocks = (f32)vd * (f32)world::CHUNK_SIZE;
    camera_.setFog(vdBlocks * 0.55f, vdBlocks * 0.94f);
    // Отладочный вид террейна из settings.cfg: debug_shading = 1..6.
    // Пока идёт развёртка, вид задаёт она: одна из её ступеней меряет
    // ландшафт с ранним выходом из фрагментного шейдера, и разность с
    // обычной ступенью даёт цену всей математики фрагмента.
    camera_.setDebugShading(passSweep_.active()
                            ? (i32)passSweep_.shading()
                            : config::settingsConst().debugShading);
    // Порядок непрозрачных чанков задаёт только развёртка, и только
    // ради замера: в игре он всегда от ближнего к дальнему, чтобы
    // работал ранний тест глубины.
    chunkRenderer_.setFarFirst(passSweep_.farFirst());
    // Расстояния мир и рендер меряют от ОДНОЙ величины — от
    // положения камеры. Раньше мир мерил от чанка, в котором стоит
    // игрок, а рендер — от камеры до центра чанка, и порядок
    // загрузки расходился с порядком отрисовки на половину чанка.
    world.setCameraPosition(camPos);

    // ---- Источники света ----
    //
    // Собираются здесь, а не в шейдере и не в мире: шейдеру нужен
    // готовый короткий список ближних, а мир о свете не знает вовсе.
    // Блоки-светильники перебираются не каждый кадр — они не
    // двигаются; факел в руке добавляется каждый кадр, потому что
    // двигается он постоянно.
    lights_.beginFrame();
    lights_.scan(world, camPos, dt);

    // Факел в руке: активная ячейка пояса, если в ней светильник.
    if (player)
        lights_.addTransient(LightField::heldLight(
            player->selectedBlock(), player->controller.state().position));

    {
        PointLight chosen[MAX_GPU_LIGHTS];
        const u32 n = lights_.nearest(camPos, chosen, MAX_GPU_LIGHTS);
        camera_.setLights(chosen, n);
    }

    // Камера в буфер здесь НЕ пишется — см. render(). Эта функция
    // выполняется до vkWaitForFences на заборе текущего кадра, то есть
    // в момент, когда GPU ещё может читать буфер этого же слота из
    // кадра, отправленного двумя кадрами раньше.
}

void RenderSystem::render(vk::Context& ctx) {
    const u32 frame = ctx.frameInFlight();

    // Матрицы камеры пишем здесь, а не в prepareFrame.
    //
    // Буферов камеры столько же, сколько кадров в работе, и выбираются
    // они по номеру кадра. Значит слот, в который мы пишем сейчас,
    // последний раз читался кадром, отправленным двумя кадрами назад —
    // и дождаться его можно только на заборе. Забор ждёт beginFrame(),
    // а prepareFrame() выполняется ДО него.
    //
    // То есть процессор переписывал матрицы прямо во время того, как
    // GPU рисовал ими предыдущий кадр. Одна отправка читает буфер не
    // разом, а по мере выполнения команд: небо и ближние чанки
    // успевали взять старую матрицу, дальние — уже новую. Геометрия
    // переставала сходиться между собой, и сквозь ближние поверхности
    // становились видны внутренности дальних. Чем быстрее двигалась
    // камера, тем заметнее.
    //
    // render() вызывается только после успешного beginFrame(), то есть
    // после ожидания на заборе: здесь слот заведомо свободен.
    CameraUbo ubo = camera_.toUbo(timeSec_);
    uboBuffers_[frame].write(&ubo, sizeof(CameraUbo));

    VkDescriptorSet ds = descriptors_.set(frame);
    math::Frustum fr = camera_.frustum();

    // Порядок кадра: сперва ВСЁ непрозрачное, потом небо, потом
    // полупрозрачное. Раньше он был другим — небо первым, вода сразу
    // за ландшафтом, сущности после воды, — и это стоило двух вещей
    // сразу.
    //
    // 1. Небо закрывает весь экран. Пока оно рисовалось первым и без
    //    проверки глубины, его шейдер считался для каждого пикселя, а
    //    ландшафт закрашивал под две трети посчитанного (доля
    //    измерена на кадре tools/vkcheck: 323777 пикселей геометрии
    //    из 504000). Шейдер неба при этом в десять раз дороже ровной
    //    заливки той же площади (tools/gpubench, кадр 2306x1080:
    //    8.08 мс против 0.78 мс). Теперь небо идёт последним из
    //    непрозрачного и с проверкой глубины: закрытые пиксели
    //    отбрасываются до фрагментного шейдера.
    //
    // 2. Вода не пишет глубину — иначе смешивание не складывается.
    //    Пока она рисовалась ДО мобов, NPC, предметов и травы, любая
    //    из этих сущностей, стоящая под водой, проходила проверку
    //    глубины (вода её не заняла) и оказывалась нарисованной
    //    ПОВЕРХ водной глади. Теперь вода идёт после них.
    // Проходы можно выключать по одному (render_passes в settings.cfg).
    // Это измерительный инструмент: цена прохода на плиточном GPU
    // честно меряется только вычитанием полного времени кадра — см.
    // комментарий к vk::Context::markPass.
    using Pass = vk::Context::GpuPass;
    // Пока идёт развёртка, маску задаёт она: иначе два источника
    // спорили бы за одно и то же поле, и замер сравнивал бы не то,
    // что думает.
    const u32 mask = passSweep_.active() ? passSweep_.mask()
                                         : config::settingsConst().renderPasses;
    auto on = [mask](Pass p) { return (mask & (1u << (u32)p)) != 0; };

    stats_ = FrameStats{};

    if (on(Pass::Terrain)) {
        chunkRenderer_.renderOpaque(ctx, voxelPipeline_.handle(),
                                    voxelPipeline_.layout(),
                                    ds, fr, camera_.position());
        stats_.drawCalls[(u32)Pass::Terrain] = chunkRenderer_.lastDrawnChunks();
    } else {
        // Отбор всё равно нужен: по нему считаются видимые чанки, и
        // без него выключение прохода меняло бы не только рисование,
        // но и числа в сводке.
        chunkRenderer_.cullOnly(fr, camera_.position());
    }
    ctx.markPass(Pass::Terrain);

    if (on(Pass::Entities)) {
        npcRenderer_.render(ctx, ds);
        mobRenderer_.render(ctx, ds, fr);
        projRenderer_.render(ctx, ds);
        itemRenderer_.render(ctx, ds);
        stats_.drawCalls[(u32)Pass::Entities] =
            (npcRenderer_.instanceCount()  ? 1u : 0u) +
            (mobRenderer_.instanceCount()  ? 1u : 0u) +
            (projRenderer_.instanceCount() ? 1u : 0u) +
            (itemRenderer_.instanceCount() ? 1u : 0u);
    }
    ctx.markPass(Pass::Entities);

    if (on(Pass::Sky)) {
        skybox_.render(ctx, ds);
        stats_.drawCalls[(u32)Pass::Sky] = 1;
    }
    ctx.markPass(Pass::Sky);

    if (on(Pass::Water)) {
        chunkRenderer_.renderBlended(ctx, voxelBlendPipeline_.handle(),
                                     voxelPipeline_.layout(), ds);
        stats_.drawCalls[(u32)Pass::Water] = chunkRenderer_.lastBlendedChunks();
    }
    ctx.markPass(Pass::Water);

    if (on(Pass::Overlay)) {
        blockOutline_.render(ctx, ds, lastHit_.block, lastHit_.hit);
        stats_.drawCalls[(u32)Pass::Overlay] = lastHit_.hit ? 1u : 0u;
    }
    ctx.markPass(Pass::Overlay);

    if (on(Pass::Ui) && ui_ && currentPlayer_ && currentWorld_) {
        ui_->render(ctx, *currentPlayer_, *currentWorld_, currentFps_);
        stats_.drawCalls[(u32)Pass::Ui] = ui_->lastDrawCalls();
    }
    ctx.markPass(Pass::Ui);
}

void RenderSystem::shutdown() {
    itemRenderer_.destroy();
    npcRenderer_.destroy();
    projRenderer_.destroy();
    mobRenderer_.destroy();
    blockOutline_.destroy();
    skybox_.destroy();
    chunkRenderer_.shutdown();
    for (auto& b : uboBuffers_) b.destroy();
    descriptors_.destroy();
    voxelBlendPipeline_.destroy();
    voxelPipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render
