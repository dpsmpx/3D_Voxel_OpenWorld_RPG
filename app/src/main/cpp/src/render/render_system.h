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
#include "chunk_renderer.h"
#include "occlusion.h"
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
                      f32 timeSec,
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

    u32 drawnChunks()   const { return chunkRenderer_.lastDrawnChunks(); }
    u32 occludedChunks()const { return occlusion_.lastCulled(); }
    u32 drawnIndices()  const { return chunkRenderer_.lastDrawnIndices(); }
    u32 lodCount(u32 l) const { return chunkRenderer_.lastLodCounts((int)l); }
    u32 grassCount()    const { return grass_.instanceCount(); }
    u32 mobInstances()  const { return mobRenderer_.instanceCount(); }
    u32 projInstances() const { return projRenderer_.instanceCount(); }
    u32 npcInstances()  const { return npcRenderer_.instanceCount(); }
    u32 itemInstances() const { return itemRenderer_.instanceCount(); }

private:
    vk::ShaderCache       shaders_;
    vk::DescriptorSet     descriptors_;
    vk::GraphicsPipeline  voxelPipeline_;
    vk::Texture2D         atlas_;
    vk::Buffer            uboBuffers_[vk::Context::MAX_FRAMES];

    ChunkRenderer         chunkRenderer_;
    OcclusionCuller       occlusion_;
    f32                   occlusionTimer_ = 0.f;
    Skybox                skybox_;
    InstancedRenderer     grass_;
    BlockOutline          blockOutline_;
    MobRenderer           mobRenderer_;
    ProjectileRenderer    projRenderer_;
    NpcRenderer           npcRenderer_;
    ItemRenderer          itemRenderer_;

    Camera                camera_;

    ui::UiSystem*         ui_ = nullptr;
    ui::Minimap*          minimap_ = nullptr;
    player::Player*       currentPlayer_ = nullptr;
    world::ChunkManager*  currentWorld_ = nullptr;
    f32                   currentFps_ = 0.f;
    f32                   timeSec_ = 0.f;
    physics::RayHit       lastHit_{};

    VkDevice              dev_ = VK_NULL_HANDLE;
};

} // namespace render
