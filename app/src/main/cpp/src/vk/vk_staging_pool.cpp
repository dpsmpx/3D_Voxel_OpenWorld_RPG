#include "vk_staging_pool.h"
#include "../core/log.h"
#include <algorithm>

namespace vk {

u32 StagingPool::findMemoryType(VkPhysicalDevice phys, u32 bits, VkMemoryPropertyFlags p) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & p) == p)
            return i;
    return 0xFFFFFFFFu;
}

bool StagingPool::init(VkDevice dev, VkPhysicalDevice phys) {
    dev_ = dev; phys_ = phys;
    return true;
}

bool StagingPool::resize(StagingBuffer* s, u64 newCap) {
    if (s->mapped) { vkUnmapMemory(dev_, s->memory); s->mapped = nullptr; }
    if (s->buffer) { vkDestroyBuffer(dev_, s->buffer, nullptr); s->buffer = VK_NULL_HANDLE; }
    if (s->memory) { vkFreeMemory(dev_, s->memory, nullptr);   s->memory = VK_NULL_HANDLE; }

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = newCap;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev_, &bci, nullptr, &s->buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, s->buffer, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(phys_, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == 0xFFFFFFFFu) return false;
    if (vkAllocateMemory(dev_, &mai, nullptr, &s->memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev_, s->buffer, s->memory, 0);
    if (vkMapMemory(dev_, s->memory, 0, newCap, 0, (void**)&s->mapped) != VK_SUCCESS) return false;

    s->capacity = newCap;
    return true;
}

StagingBuffer* StagingPool::acquire(u64 minSize) {
    // 1. Свободный и достаточно большой
    for (auto& b : buffers_)
        if (!b->inUse && b->capacity >= minSize) { b->inUse = true; return b.get(); }

    // 2. Свободный, но маленький — расширяем
    for (auto& b : buffers_) {
        if (!b->inUse && b->capacity < minSize) {
            u64 cap = nextCapacity_;
            while (cap < minSize) cap *= 2;
            if (resize(b.get(), cap)) { nextCapacity_ = std::max(nextCapacity_, cap); b->inUse = true; return b.get(); }
        }
    }

    // 3. Новый
    u64 cap = nextCapacity_;
    while (cap < minSize) cap *= 2;
    auto b = std::make_unique<StagingBuffer>();
    if (!resize(b.get(), cap)) return nullptr;
    nextCapacity_ = std::max(nextCapacity_, cap);
    b->inUse = true;
    buffers_.push_back(std::move(b));
    return buffers_.back().get();
}

u64 StagingPool::totalBytes() const {
    u64 sum = 0;
    for (auto& b : buffers_) sum += b->capacity;
    return sum;
}

void StagingPool::destroy() {
    for (auto& b : buffers_) {
        if (b->mapped) vkUnmapMemory(dev_, b->memory);
        if (b->buffer) vkDestroyBuffer(dev_, b->buffer, nullptr);
        if (b->memory) vkFreeMemory(dev_, b->memory, nullptr);
    }
    buffers_.clear();
}

} // namespace vk