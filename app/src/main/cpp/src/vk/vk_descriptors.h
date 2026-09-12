/**
 * @file vk_descriptors.h
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#pragma once
#include "../core/types.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace vk {

/// DescriptorSet — layout + pool + per-frame sets.
/// На кадр: UBO (binding 0) + atlas (binding 1).
class DescriptorSet {
public:
    bool create(VkDevice dev, u32 framesInFlight);
    void destroy();

    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet       set(u32 frame) const { return sets_[frame]; }

    /// Привязать ресурсы к конкретному сету кадра.
    void bindUbo(u32 frame, VkBuffer ubo, u64 size);

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool     pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets_;
    u32                  frames_ = 2;
};

} // namespace vk
