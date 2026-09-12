/**
 * @file vk_staging_pool.cpp
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#include "vk_staging_pool.h"
#include "../core/log.h"
#include <algorithm>
#include <memory>

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

void StagingPool::collect(u32 keep) {
    keepBatches_ = keep ? keep : 1;
    ++batchNo_;
    for (auto& b : buffers_)
        if (b->inUse && b->freeAtBatch != 0 && batchNo_ >= b->freeAtBatch) {
            b->inUse = false;
            b->freeAtBatch = 0;
        }
}

void StagingPool::retire(StagingBuffer* s) {
    if (!s) return;
    // Освободится не раньше, чем через keepBatches_ пакетов: столько
    // держится кольцо командных буферов передачи в vk::Context.
    s->freeAtBatch = batchNo_ + keepBatches_;
}

bool StagingPool::resize(StagingBuffer* s, u64 newCap) {
    // Буфер сейчас перестанет существовать. Если пересоздать его не
    // удастся, ёмкость должна быть нулевой: иначе acquire() потом
    // выдаст «достаточно большой» буфер с пустым дескриптором, и
    // копия пойдёт из ниоткуда.
    s->capacity = 0;
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

/// Размер буфера под запрос: степень двойки, но не меньше порога.
/// Раньше здесь стояла общая на весь пул «текущая ёмкость», которая
/// только росла: один большой чанк поднимал её, и дальше КАЖДЫЙ
/// буфер выделялся по новому размеру. При минимуме в мегабайт и
/// нескольких десятках живых буферов это десятки мегабайт под данные,
/// которым хватает пары.
static u64 capacityFor(u64 minSize) {
    u64 cap = 64ull * 1024;
    while (cap < minSize) cap *= 2;
    return cap;
}

StagingBuffer* StagingPool::acquire(u64 minSize) {
    if (minSize == 0) return nullptr;

    // 1. Свободный и достаточно большой — берём наименьший подходящий,
    //    чтобы четырёхмегабайтный буфер не уходил под сорок килобайт.
    StagingBuffer* best = nullptr;
    for (auto& b : buffers_) {
        if (b->inUse || b->capacity < minSize) continue;
        if (!best || b->capacity < best->capacity) best = b.get();
    }
    if (best) { best->inUse = true; return best; }

    // 2. Свободный, но маленький — расширяем самый большой из них:
    //    так растёт один буфер, а не плодятся новые.
    StagingBuffer* grow = nullptr;
    for (auto& b : buffers_) {
        if (b->inUse) continue;
        if (!grow || b->capacity > grow->capacity) grow = b.get();
    }
    if (grow && resize(grow, capacityFor(minSize))) { grow->inUse = true; return grow; }

    // 3. Новый
    auto b = std::make_unique<StagingBuffer>();
    if (!resize(b.get(), capacityFor(minSize))) return nullptr;
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
