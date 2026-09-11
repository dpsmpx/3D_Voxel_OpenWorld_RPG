#pragma once
#include "../core/types.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

namespace vk {

struct StagingBuffer {
    VkBuffer       buffer   = VK_NULL_HANDLE;
    VkDeviceMemory memory   = VK_NULL_HANDLE;
    u8*            mapped   = nullptr;
    u64            capacity = 0;
    bool           inUse    = false;
};

// ============================================================
// StagingPool — переиспользует staging-буферы между upload'ами.
// Аллокация vkAllocateMemory происходит редко (только рост capacity).
// Предполагается, что вызывающий код синхронно дожидается завершения
// GPU-работы перед release() (в нашем случае — submitOneShot).
// Не потокобезопасен — вызовы только с главного потока.
// ============================================================
class StagingPool {
public:
    bool init(VkDevice dev, VkPhysicalDevice phys);
    void destroy();

    StagingBuffer* acquire(u64 minSize);
    void release(StagingBuffer* s) { if (s) s->inUse = false; }

    usize count() const { return buffers_.size(); }
    u64 totalBytes() const;

private:
    static u32 findMemoryType(VkPhysicalDevice phys, u32 bits, VkMemoryPropertyFlags p);
    bool resize(StagingBuffer* s, u64 newCap);

    VkDevice         dev_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<StagingBuffer>> buffers_;
    u64 nextCapacity_ = 1ull << 20;  // стартовый размер 1 MiB
};

} // namespace vk
