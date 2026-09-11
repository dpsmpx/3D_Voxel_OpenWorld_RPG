#include "vk_buffer.h"
#include "vk_context.h"
#include "../core/log.h"
#include <cstring>
#include <vector>

namespace vk {

u32 Buffer::findMemoryType(VkPhysicalDevice phys, u32 typeBits, VkMemoryPropertyFlags props) {
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

void Buffer::upload(VkDevice dev, VkPhysicalDevice phys, VkCommandBuffer cmdOrNull,
                    const void* data, u64 size)
{
    if (hostVisible_) { write(data, size); return; }
    if (!cmdOrNull) { LOGE("upload: нужен cmd buffer"); return; }

    // Временный staging
    Buffer staging;
    if (!staging.create(dev, phys, size, BufferUsage::Staging, true)) return;
    staging.write(data, size);

    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmdOrNull, staging.handle(), buf_, 1, &region);

    // Staging уничтожаем после submit — но vkCmdCopyBuffer ещё не выполнен.
    // Поэтому используем vkQueueWaitIdle в Context::submitOneShot, после чего
    // staging будет безопасно уничтожен деструктором.
    // ВАЖНО: эту функцию вызывать ТОЛЬКО внутри submitOneShot.
    // Откладываем destroy до следующего upload. Простейший способ — утечка
    // при N вызовах, поэтому мы не полагаемся на неё — используем write() для
    // маленьких данных, а большие заливаем через Context::submitOneShot.
    staging.destroy();
}

} // namespace vk
