/**
 * @file vk_texture.cpp
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#include "vk_texture.h"
#include "../core/log.h"
#include <cstring>
#include <algorithm>

namespace vk {

u32 Texture2D::findMemoryType(VkPhysicalDevice phys, u32 bits, VkMemoryPropertyFlags p) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & p) == p) return i;
    return 0xFFFFFFFFu;
}

u32 Texture2D::mipCount(u32 w, u32 h) {
    u32 n = 1;
    while (w > 1 || h > 1) { w = std::max(1u, w/2); h = std::max(1u, h/2); ++n; }
    return n;
}

bool Texture2D::isCompressedFormat(VkFormat fmt) {
    // Диапазоны из спецификации Vulkan: BC, ETC2/EAC и ASTC LDR.
    return (fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK &&
            fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

bool Texture2D::create(VkDevice dev, VkPhysicalDevice phys, VkQueue queue, u32 queueFamily,
                       u32 w, u32 h, VkFormat fmt, const void* pixels, u64 bytes,
                       VkFilter filter, VkSamplerAddressMode addr, bool mipGen)
{
    // Нулевой дескриптор драйвер разыменует и уронит процесс внутри
    // libvulkan, где ни имени нашей функции, ни строки уже не видно.
    if (dev == VK_NULL_HANDLE || phys == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        LOGE("Texture2D::create: устройство=%p, физическое=%p, очередь=%p — текстура не создана",
             (void*)dev, (void*)phys, (void*)queue);
        return false;
    }
    if (w == 0 || h == 0) {
        LOGE("Texture2D::create: нулевой размер %ux%u", w, h);
        return false;
    }

    dev_ = dev; width_ = w; height_ = h; format_ = fmt;

    // Мип-цепочка строится через vkCmdBlitImage, а он не работает
    // со сжатыми форматами: у них уровень всегда один.
    if (mipGen && isCompressedFormat(fmt)) {
        mipGen = false;
        LOGI("Texture2D: формат сжатый, мипы не генерируются на устройстве");
    }
    mipLevels_ = mipGen ? mipCount(w, h) : 1;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = fmt;
    ici.extent      = { w, h, 1 };
    ici.mipLevels   = mipLevels_;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipGen) ici.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, &image_) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == 0xFFFFFFFFu ||
        vkAllocateMemory(dev, &mai, nullptr, &mem_) != VK_SUCCESS) {
        LOGE("Texture2D::create: память под изображение %ux%u не выделена", w, h);
        destroy();
        return false;
    }
    if (vkBindImageMemory(dev, image_, mem_, 0) != VK_SUCCESS) { destroy(); return false; }

    // Промежуточный буфер. Каждый шаг проверяется: раньше результаты
    // не спрашивались вовсе, и неудавшееся отображение памяти
    // оставляло указатель неинициализированным — пиксели копировались
    // по случайному адресу.
    VkBuffer sBuf = VK_NULL_HANDLE; VkDeviceMemory sMem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    auto fail = [&](const char* what) {
        LOGE("Texture2D::create: %s", what);
        if (pool) vkDestroyCommandPool(dev, pool, nullptr);
        if (sBuf) vkDestroyBuffer(dev, sBuf, nullptr);
        if (sMem) vkFreeMemory(dev, sMem, nullptr);
        destroy();
        return false;
    };
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(dev, &bci, nullptr, &sBuf) != VK_SUCCESS) {
            sBuf = VK_NULL_HANDLE;
            return fail("промежуточный буфер не создан");
        }
        VkMemoryRequirements r2; vkGetBufferMemoryRequirements(dev, sBuf, &r2);
        VkMemoryAllocateInfo m2{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        m2.allocationSize = r2.size;
        m2.memoryTypeIndex = findMemoryType(phys, r2.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (m2.memoryTypeIndex == 0xFFFFFFFFu ||
            vkAllocateMemory(dev, &m2, nullptr, &sMem) != VK_SUCCESS) {
            sMem = VK_NULL_HANDLE;
            return fail("память промежуточного буфера не выделена");
        }
        if (vkBindBufferMemory(dev, sBuf, sMem, 0) != VK_SUCCESS)
            return fail("память промежуточного буфера не привязана");
        void* m = nullptr;
        if (vkMapMemory(dev, sMem, 0, bytes, 0, &m) != VK_SUCCESS || !m)
            return fail("промежуточный буфер не отображён");
        std::memcpy(m, pixels, bytes);
        vkUnmapMemory(dev, sMem);
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) {
        pool = VK_NULL_HANDLE;
        return fail("пул команд не создан");
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &cbai, &cmd) != VK_SUCCESS)
        return fail("командный буфер не выделен");

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    auto transition = [&](VkImageLayout from, VkImageLayout to,
                          VkAccessFlags sa, VkAccessFlags da,
                          VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                          u32 baseMip, u32 count) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image_;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = baseMip;
        b.subresourceRange.levelCount = count;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = sa; b.dstAccessMask = da;
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               0, mipLevels_);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(cmd, sBuf, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (mipGen && mipLevels_ > 1) {
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   0, 1);

        i32 mw = (i32)w, mh = (i32)h;
        for (u32 i = 1; i < mipLevels_; ++i) {
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = { mw, mh, 1 };
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.layerCount = 1;
            i32 nw = std::max(1, mw/2), nh = std::max(1, mh/2);
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = { nw, nh, 1 };

            transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       i, 1);

            vkCmdBlitImage(cmd,
                image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, VK_FILTER_LINEAR);

            transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       i, 1);

            mw = nw; mh = nh;
        }

        transition(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   0, mipLevels_);
    } else {
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   0, 1);
    }

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, sBuf, nullptr);
    vkFreeMemory(dev, sMem, nullptr);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = mipLevels_;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &vci, nullptr, &view_) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = filter;
    sci.minFilter = filter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = addr; sci.addressModeV = addr; sci.addressModeW = addr;
    sci.maxLod = (f32)mipLevels_;
    sci.anisotropyEnable = VK_FALSE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler_) != VK_SUCCESS) return false;

    LOGI("Texture2D: %ux%u mips=%u fmt=%d", w, h, mipLevels_, (int)fmt);
    return true;
}

// ============================================================
// Phase 15: upload
// ============================================================
bool Texture2D::upload(VkDevice dev, VkPhysicalDevice phys,
                       VkCommandPool pool, VkQueue queue,
                       const void* pixels, u64 byteSize)
{
    if (!image_) return false;

    // Staging buffer
    VkBuffer sBuf = VK_NULL_HANDLE;
    VkDeviceMemory sMem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = byteSize; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(dev, &bci, nullptr, &sBuf);
        VkMemoryRequirements r2; vkGetBufferMemoryRequirements(dev, sBuf, &r2);
        VkMemoryAllocateInfo m2{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        m2.allocationSize = r2.size;
        m2.memoryTypeIndex = findMemoryType(phys, r2.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(dev, &m2, nullptr, &sMem);
        vkBindBufferMemory(dev, sBuf, sMem, 0);
        void* m; vkMapMemory(dev, sMem, 0, byteSize, 0, &m);
        std::memcpy(m, pixels, byteSize);
        vkUnmapMemory(dev, sMem);
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd; vkAllocateCommandBuffers(dev, &cbai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // Transition: SHADER_READ_ONLY -> TRANSFER_DST
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image_;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width_, height_, 1 };
    vkCmdCopyBufferToImage(cmd, sBuf, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition back: TRANSFER_DST -> SHADER_READ_ONLY
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image_;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vkDestroyBuffer(dev, sBuf, nullptr);
    vkFreeMemory(dev, sMem, nullptr);
    return true;
}

void Texture2D::destroy() {
    if (!dev_) return;
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
    if (view_)    vkDestroyImageView(dev_, view_, nullptr);
    if (image_)   vkDestroyImage(dev_, image_, nullptr);
    if (mem_)     vkFreeMemory(dev_, mem_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    view_    = VK_NULL_HANDLE;
    image_   = VK_NULL_HANDLE;
    mem_ = VK_NULL_HANDLE;
    dev_ = VK_NULL_HANDLE;
}

} // namespace vk
