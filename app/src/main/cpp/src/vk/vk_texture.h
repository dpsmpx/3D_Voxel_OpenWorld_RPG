#pragma once
#include "../core/types.h"
#include <vulkan/vulkan.h>

namespace vk {

class Texture2D {
public:
    bool create(VkDevice dev, VkPhysicalDevice phys, VkQueue queue, u32 queueFamily,
                u32 width, u32 height, VkFormat format,
                const void* pixels, u64 byteSize,
                VkFilter filter = VK_FILTER_NEAREST,
                VkSamplerAddressMode addr = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                bool generateMips = false);

    void destroy();

    // Phase 15: обновление содержимого текстуры через staging buffer
    // и vkCmdCopyBufferToImage. Формат — должен совпадать с исходным.
    bool upload(VkDevice dev, VkPhysicalDevice phys,
                VkCommandPool pool, VkQueue queue,
                const void* pixels, u64 byteSize);

    VkImage     image()      const { return image_; }
    VkImageView view()       const { return view_; }
    VkSampler   sampler()    const { return sampler_; }
    u32         width()      const { return width_; }
    u32         height()     const { return height_; }
    u32         mipLevels()  const { return mipLevels_; }

private:
    static u32 findMemoryType(VkPhysicalDevice phys, u32 bits, VkMemoryPropertyFlags p);
    static u32 mipCount(u32 w, u32 h);

    VkDevice       dev_ = VK_NULL_HANDLE;
    VkImage        image_ = VK_NULL_HANDLE;
    VkDeviceMemory mem_   = VK_NULL_HANDLE;
    VkImageView    view_  = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;
    u32 width_ = 0, height_ = 0, mipLevels_ = 1;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
};

} // namespace vk
