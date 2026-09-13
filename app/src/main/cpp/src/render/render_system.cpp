/**
 * @file render_system.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
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
    if (!grass_.init(ctx, mgr, descriptors_.layout())) {
        LOGW("InstancedRenderer не инициализирован");
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
    chunkRenderer_.uploadChunks(ctx, world, ready, camera_.position());

    mobRenderer_.rebuild(registry);
    mobRenderer_.upload(ctx);

    npcRenderer_.rebuild(registry);
    npcRenderer_.upload(ctx);

    projRenderer_.rebuild(registry);
    projRenderer_.upload(ctx);

    itemRenderer_.rebuild(registry);
    itemRenderer_.upload(ctx);

    // Трава пересобирается по шести сотням проб шума вокруг игрока.
    // Раньше это делалось строго раз в полсекунды, даже когда игрок
    // стоит на месте и пересобирать нечего — ровный всплеск работы
    // дважды в секунду на ровном месте. Теперь поводов два: игрок
    // заметно сдвинулся, либо прошло достаточно времени, чтобы
    // подхватить изменения рельефа от построек и раскопок.
    // Счётчик — поле, а не статическая переменная функции: та
    // переживала смену мира и путала первый кадр нового.
    grassTimer_ += dt;
    const glm::vec3 camPos = camera_.position();
    const f32 grassDx = camPos.x - grassOrigin_.x;
    const f32 grassDz = camPos.z - grassOrigin_.z;
    const bool grassMoved = grassDx * grassDx + grassDz * grassDz > 6.f * 6.f;
    if (grass_.instanceCount() == 0 ||
        (grassTimer_ > 0.5f && grassMoved) || grassTimer_ > 3.f) {
        grassTimer_  = 0.f;
        grassOrigin_ = camPos;
        grass_.populateGrass(world, camPos, 40.f);
        grass_.upload(ctx);
    }

    // Применяем настройки viewDistance к миру
    const i32 vd = config::settingsConst().viewDistance;
    world.setViewDistance(vd);

    // Туман привязан к дальности прорисовки, а не к постоянным числам.
    // Иначе он кончается там, где мир ещё есть (или наоборот), и край
    // загруженных чанков виден обрывом на фоне чистого неба.
    const f32 vdBlocks = (f32)vd * (f32)world::CHUNK_SIZE;
    camera_.setFog(vdBlocks * 0.55f, vdBlocks * 0.94f);
    chunkRenderer_.setViewDistanceBlocks(vdBlocks);
    // Отладочный вид террейна из settings.cfg: debug_shading = 1..6.
    camera_.setDebugShading(config::settingsConst().debugShading);
    world.setLodBands(chunkRenderer_.lodBand(0), chunkRenderer_.lodBand(1),
                      chunkRenderer_.lodBand(2));

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

    skybox_.render(ctx, ds);

    chunkRenderer_.render(ctx, voxelPipeline_.handle(),
                          voxelBlendPipeline_.handle(), voxelPipeline_.layout(),
                          ds, fr, camera_.position());

    npcRenderer_.render(ctx, ds);
    mobRenderer_.render(ctx, ds, fr);
    projRenderer_.render(ctx, ds);
    itemRenderer_.render(ctx, ds);
    grass_.render(ctx, ds, fr);

    blockOutline_.render(ctx, ds, lastHit_.block, lastHit_.hit);

    if (ui_ && currentPlayer_ && currentWorld_) {
        ui_->render(ctx, *currentPlayer_, *currentWorld_, currentFps_);
    }
}

void RenderSystem::shutdown() {
    itemRenderer_.destroy();
    npcRenderer_.destroy();
    projRenderer_.destroy();
    mobRenderer_.destroy();
    blockOutline_.destroy();
    grass_.destroy();
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
