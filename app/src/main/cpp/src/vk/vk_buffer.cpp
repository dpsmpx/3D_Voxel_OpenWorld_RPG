/**
 * @file vk_buffer.cpp
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#include "vk_buffer.h"
#include "vk_context.h"
#include "../core/log.h"
#include <cstring>
#include <vector>

namespace vk {

u32 Buffer::findMemoryType(VkPhysicalDevice phys, u32 typeBits, VkMemoryPropertyFlags props) {
    // Нулевой дескриптор драйвер не проверяет: он разыменовывает его и
    // роняет процесс внутри libvulkan, где от нашего кода не остаётся
    // ни имени функции, ни строки. Отвергаем сами.
    if (phys == VK_NULL_HANDLE) {
        LOGE("findMemoryType: физическое устройство не задано");
        return 0xFFFFFFFFu;
    }

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0xFFFFFFFFu;
}

bool Buffer::create(VkDevice dev, VkPhysicalDevice phys,
                    u64 size, BufferUsage usage, bool hostVisible)
{
    if (dev == VK_NULL_HANDLE || phys == VK_NULL_HANDLE) {
        LOGE("Buffer::create: устройство=%p, физическое устройство=%p — буфер не создан",
             (void*)dev, (void*)phys);
        return false;
    }
    if (size == 0) {
        LOGE("Buffer::create: нулевой размер");
        return false;
    }

    dev_ = dev;
    size_ = size;
    hostVisible_ = hostVisible;

    VkBufferUsageFlags vkUsage = 0;
    switch (usage) {
        case BufferUsage::Vertex:  vkUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; break;
        case BufferUsage::Index:   vkUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT; break;
        case BufferUsage::Uniform: vkUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; break;
        case BufferUsage::Staging: vkUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; break;
    }

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size        = size;
    bci.usage       = vkUsage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(dev, &bci, nullptr, &buf_) != VK_SUCCESS) {
        LOGE("vkCreateBuffer fail, size=%llu", (unsigned long long)size);
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf_, &req);

    VkMemoryPropertyFlags flags = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(phys, req.memoryTypeBits, flags);
    if (mai.memoryTypeIndex == 0xFFFFFFFFu) {
        LOGE("Подходящий memory type не найден");
        vkDestroyBuffer(dev, buf_, nullptr);
        buf_ = VK_NULL_HANDLE;
        return false;
    }

    if (vkAllocateMemory(dev, &mai, nullptr, &mem_) != VK_SUCCESS) {
        LOGE("vkAllocateMemory fail");
        vkDestroyBuffer(dev, buf_, nullptr);
        buf_ = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(dev, buf_, mem_, 0);

    if (hostVisible_) {
        if (vkMapMemory(dev, mem_, 0, size_, 0, &mapped_) != VK_SUCCESS) {
            LOGE("vkMapMemory fail");
            return false;
        }
    }
    return true;
}

Buffer::Handles Buffer::release() {
    if (mapped_) { vkUnmapMemory(dev_, mem_); mapped_ = nullptr; }
    Handles h{ dev_, buf_, mem_ };
    dev_ = VK_NULL_HANDLE;
    buf_ = VK_NULL_HANDLE;
    mem_ = VK_NULL_HANDLE;
    size_ = 0;
    hostVisible_ = false;
    return h;
}

void Buffer::destroy() {
    if (!dev_) return;
    if (mapped_) { vkUnmapMemory(dev_, mem_); mapped_ = nullptr; }
    if (buf_) { vkDestroyBuffer(dev_, buf_, nullptr); buf_ = VK_NULL_HANDLE; }
    if (mem_) { vkFreeMemory(dev_, mem_, nullptr); mem_ = VK_NULL_HANDLE; }
    dev_ = VK_NULL_HANDLE;
}

void* Buffer::map() { return mapped_; }
void  Buffer::unmap() {
    if (dev_ && mapped_) { vkUnmapMemory(dev_, mem_); mapped_ = nullptr; }
}

void Buffer::write(const void* data, u64 size) {
    if (!hostVisible_ || !mapped_) return;
    u64 n = size < size_ ? size : size_;
    std::memcpy(mapped_, data, n);
}

} // namespace vk
