/**
 * @file render_system.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "../vk/vk_descriptors.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_texture.h"
#include "../vk/vk_buffer.h"
#include "../ecs/registry.h"
#include "../physics/raycast.h"
#include "../player/player.h"
#include "../world/chunk_manager.h"
#include "camera.h"
#include "pass_sweep.h"
#include "chunk_renderer.h"
#include "skybox.h"
#include "instanced_renderer.h"
#include "block_outline.h"
#include "mob_renderer.h"
#include "projectile_renderer.h"
#include "npc_renderer.h"
#include "item_renderer.h"
#include "../ui/ui_system.h"
#include <android/asset_manager.h>

namespace render {


class RenderSystem {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr);
    void shutdown();

    void prepareFrame(vk::Context& ctx,
                      world::ChunkManager& world,
                      ecs::Registry& registry,
                      f32 timeSec, f32 dt,
                      player::Player* player,
                      const physics::RayHit& targetHit,
                      f32 fps);

    void render(vk::Context& ctx);

    void setUiSystem(ui::UiSystem* u) { ui_ = u; }

    void forgetChunk(world::ChunkCoord coord) {
        chunkRenderer_.forgetChunk(coord);
    }

    // Проброс к миникарте (использует main)
    ui::Minimap* minimap() { return minimap_; }
    void setMinimap(ui::Minimap* m) { minimap_ = m; }

    Camera& camera() { return camera_; }

    /// Счётчики кадра по проходам. Время GPU лежит не здесь, а в
    /// vk::Context (метки ставит сам GPU); тут — то, что знает
    /// процессор: сколько команд рисования он записал.
    struct FrameStats {
        u32 drawCalls[vk::Context::GPU_PASSES] = {};
        u32 total() const {
            u32 n = 0;
            for (u32 d : drawCalls) n += d;
            return n;
        }
    };
    const FrameStats& stats() const { return stats_; }

    /// Развёртка по проходам: выключает их по одному и печатает
    /// таблицу цен. Кормится временем GPU из главного цикла.
    PassSweep& passSweep() { return passSweep_; }

    u32 drawnChunks()   const { return chunkRenderer_.lastDrawnChunks(); }
    u32 consideredChunks() const { return chunkRenderer_.lastConsideredChunks(); }
    u32 culledChunks()  const { return chunkRenderer_.lastCulledChunks(); }
    u32 drawnVertices() const { return chunkRenderer_.lastDrawnVertices(); }
    u32 drawnIndices()  const { return chunkRenderer_.lastDrawnIndices(); }
    /// Чанки в кадре, которым нечем рисоваться, — это и есть дыры.
    u32 emptyChunks()   const { return chunkRenderer_.lastEmptyChunks(); }
    u32 waitingChunks() const { return chunkRenderer_.lastWaitingChunks(); }
    u32 grassCount()    const { return grass_.instanceCount(); }
    u32 mobInstances()  const { return mobRenderer_.instanceCount(); }
    u32 projInstances() const { return projRenderer_.instanceCount(); }
    u32 npcInstances()  const { return npcRenderer_.instanceCount(); }
    u32 itemInstances() const { return itemRenderer_.instanceCount(); }

private:
    vk::ShaderCache       shaders_;
    vk::DescriptorSet     descriptors_;
    vk::GraphicsPipeline  voxelPipeline_;
    /// Тот же формат вершин и те же шейдеры, но со смешиванием и без
    /// записи глубины: вода и лёд идут вторым проходом поверх мира.
    vk::GraphicsPipeline  voxelBlendPipeline_;
    vk::Buffer            uboBuffers_[vk::Context::MAX_FRAMES];

    ChunkRenderer         chunkRenderer_;
    Skybox                skybox_;
    InstancedRenderer     grass_;
    BlockOutline          blockOutline_;
    MobRenderer           mobRenderer_;
    ProjectileRenderer    projRenderer_;
    NpcRenderer           npcRenderer_;
    ItemRenderer          itemRenderer_;

    Camera                camera_;

    ui::UiSystem*         ui_ = nullptr;

    /// Когда и откуда в последний раз рассаживали траву.
    f32       grassTimer_  = 0.f;
    glm::vec3 grassOrigin_{ 0.f };
    ui::Minimap*          minimap_ = nullptr;
    player::Player*       currentPlayer_ = nullptr;
    world::ChunkManager*  currentWorld_ = nullptr;
    f32                   currentFps_ = 0.f;
    f32                   timeSec_ = 0.f;
    physics::RayHit       lastHit_{};
    FrameStats            stats_{};
    PassSweep             passSweep_{};

    VkDevice              dev_ = VK_NULL_HANDLE;
};

} // namespace render
