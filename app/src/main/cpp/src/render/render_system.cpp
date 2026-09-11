/**
 * @file render_system.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "render_system.h"
#include "atlas_builder.h"
#include "astc.h"
#include "../core/log.h"
#include "../config/settings.h"
#include <glm/glm.hpp>

namespace render {

// Вершинный формат террейна — см. VoxelVertex в mesh_builder.h.
static const vk::VertexBinding kVoxelBindings[1] = { { sizeof(VoxelVertex), false } };
static const vk::VertexAttr kVoxelAttrs[4] = {
    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // inPos
    { 1, 0, VK_FORMAT_R32G32_SFLOAT,    12 },   // inUv (в тайлах)
    { 2, 0, VK_FORMAT_R32G32_SFLOAT,    20 },   // inTileOrigin
    { 3, 0, VK_FORMAT_R8G8B8A8_UNORM,   28 },   // inColor
};

bool RenderSystem::init(vk::Context& ctx, AAssetManager* mgr) {
    dev_ = ctx.device();
    shaders_.init(dev_, mgr);

    // Атлас блоков. Сжатый ASTC 4x4 (ТЗ 3.3) экономит вчетверо
    // памяти и пропускной способности, но поддержан не везде и
    // собирается на этапе сборки утилитой tools/atlas. Если ассета
    // нет или GPU формат не тянет — собираем процедурный RGBA8.
    bool atlasReady = false;
    if (ctx.formatSupportsSampling(VK_FORMAT_ASTC_4x4_UNORM_BLOCK)) {
        const AstcImage astc = loadAstcAsset(mgr, "textures/blocks.astc");
        if (astc.valid() && astc.blockX == 4 && astc.blockY == 4) {
            atlasReady = atlas_.create(
                ctx.device(), ctx.physicalDevice(),
                ctx.gfxQueue(), ctx.gfxFamily(),
                astc.width, astc.height,
                VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
                astc.data.data(), astc.data.size(),
                VK_FILTER_LINEAR,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                false);
            if (atlasReady)
                LOGI("Атлас: ASTC 4x4, %zu КБ", astc.data.size() / 1024);
        }
    } else {
        LOGI("Атлас: ASTC 4x4 не поддержан устройством");
    }

    if (!atlasReady) {
        const AtlasData atlasData = buildProceduralAtlas();
        atlasReady = atlas_.create(
            ctx.device(), ctx.physicalDevice(),
            ctx.gfxQueue(), ctx.gfxFamily(),
            atlasData.width, atlasData.height,
            VK_FORMAT_R8G8B8A8_UNORM,
            atlasData.pixels.data(), atlasData.pixels.size(),
            VK_FILTER_NEAREST,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            true);
        if (atlasReady)
            LOGI("Атлас: процедурный RGBA8, %zu КБ",
                 atlasData.pixels.size() / 1024);
    }

    if (!atlasReady) {
        LOGE("Атлас не создан");
        return false;
    }

    if (!descriptors_.create(ctx.device(), vk::Context::MAX_FRAMES)) {
        LOGE("DescriptorSet не создан");
        return false;
    }

    for (u32 i = 0; i < vk::Context::MAX_FRAMES; ++i) {
        if (!uboBuffers_[i].create(ctx.device(), ctx.physicalDevice(),
                                   sizeof(CameraUbo),
                                   vk::BufferUsage::Uniform, true)) return false;
        descriptors_.bindUbo(i, uboBuffers_[i].handle(), sizeof(CameraUbo));
        descriptors_.bindTexture(i, atlas_.view(), atlas_.sampler());
    }

    {
        vk::PipelineDesc d{};
        d.renderPass  = ctx.renderPass();
        d.descLayout  = descriptors_.layout();
        d.vertName    = "shaders/voxel.vert.spv";
        d.fragName    = "shaders/voxel.frag.spv";
        d.depthFormat = ctx.depthFormat();
        d.cullMode    = VK_CULL_MODE_BACK_BIT;
        d.depthTest   = true;
        d.depthWrite  = true;
        d.blend       = false;
        d.bindings     = kVoxelBindings;
        d.bindingCount = 1;
        d.attrs        = kVoxelAttrs;
        d.attrCount    = 4;
        if (!voxelPipeline_.create(dev_, shaders_, d)) return false;
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
                                f32 timeSec,
                                player::Player* player,
                                const physics::RayHit& targetHit,
                                f32 fps)
{
    timeSec_       = timeSec;
    currentPlayer_ = player;
    currentWorld_  = &world;
    currentFps_    = fps;
    lastHit_       = targetHit;

    const auto ready = world.pollMeshesReady();
    chunkRenderer_.uploadChunks(ctx, ready, camera_.position());

    // Карта перекрытий обновляется раз в полсекунды: мир меняется
    // медленнее, а полный обход чанков недёшев.
    occlusionTimer_ += 1.f / 60.f;
    if (occlusionTimer_ >= 0.5f) {
        occlusionTimer_ = 0.f;
        occlusion_.rebuild(world, camera_.position(), world.viewDistance() + 1);
    }

    mobRenderer_.rebuild(registry);
    mobRenderer_.upload(ctx);

    npcRenderer_.rebuild(registry);
    npcRenderer_.upload(ctx);

    projRenderer_.rebuild(registry);
    projRenderer_.upload(ctx);

    itemRenderer_.rebuild(registry);
    itemRenderer_.upload(ctx);

    static f32 grassTimer = 0.f;
    grassTimer += (1.f / 60.f);
    if (grass_.instanceCount() == 0 || grassTimer > 0.5f) {
        grassTimer = 0.f;
        grass_.populateGrass(world, camera_.position(), 40.f);
        grass_.upload(ctx);
    }

    // Применяем настройки viewDistance к миру
    world.setViewDistance(config::settingsConst().viewDistance);

    const u32 frame = ctx.frameInFlight();
    CameraUbo ubo = camera_.toUbo(timeSec);
    uboBuffers_[frame].write(&ubo, sizeof(CameraUbo));
}

void RenderSystem::render(vk::Context& ctx) {
    const u32 frame = ctx.frameInFlight();
    VkDescriptorSet ds = descriptors_.set(frame);
    math::Frustum fr = camera_.frustum();

    VkCommandBuffer cmd = ctx.currentCmd();

    skybox_.render(ctx);

    chunkRenderer_.render(ctx, voxelPipeline_.handle(), voxelPipeline_.layout(),
                          ds, fr, camera_.position(), &occlusion_);

    npcRenderer_.render(ctx);
    mobRenderer_.render(ctx, fr);
    projRenderer_.render(ctx);
    itemRenderer_.render(ctx);

    {
        VkViewport vp{};
        vp.width  = (f32)ctx.extent().width;
        vp.height = (f32)ctx.extent().height;
        vp.minDepth = 0.f; vp.maxDepth = 1.f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{}; sc.extent = ctx.extent();
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                voxelPipeline_.layout(), 0, 1, &ds, 0, nullptr);
        grass_.render(ctx, fr);
    }

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
    atlas_.destroy();
    voxelPipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render
