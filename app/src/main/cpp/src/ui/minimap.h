#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "../vk/vk_texture.h"
#include "../world/chunk_manager.h"
#include <glm/glm.hpp>
#include <vector>

namespace ui {

class Minimap {
public:
    bool init(vk::Context& ctx, u32 px = 128);
    void destroy();

    void update(world::ChunkManager& world,
                const glm::vec3& playerPos,
                f32 radiusBlocks = 64.f);

    // Phase 15: если текстура обновилась — заливаем в GPU.
    // Вызывать из render/prepareFrame.
    void flushUpload(vk::Context& ctx);

    VkImageView view()    const { return tex_.view(); }
    VkSampler   sampler() const { return tex_.sampler(); }
    u32         size()    const { return size_; }

    f32 blocksPerPixel() const { return blocksPerPixel_; }
    const glm::vec3& center() const { return center_; }

    bool dirty() const { return dirty_; }

private:
    VkDevice          dev_ = VK_NULL_HANDLE;
    vk::Texture2D     tex_;
    std::vector<u8>   pixels_;
    u32               size_ = 128;
    f32               blocksPerPixel_ = 1.f;
    glm::vec3         center_{0.f};
    bool              dirty_ = false;
};

u32 blockMapColor(u16 blockId);

} // namespace ui