/**
 * @file vk_staging_pool.h
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
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
    /// Номер пакета, начиная с которого буфер снова свободен. GPU
    /// читает из него уже после того, как процессор ушёл дальше.
    u64            freeAtBatch = 0;
};

/// StagingPool — переиспользует staging-буферы между загрузками.
/// vkAllocateMemory случается редко — только когда нужен буфер
/// больше имеющихся.
///
/// Пакет передачи не дожидается GPU на процессоре, поэтому отданный
/// буфер нельзя занимать сразу: из него ещё читают. retire() помечает
/// буфер сроком годности в пакетах, collect() на следующем пакете
/// забирает то, что отлежалось. Без этого содержимое одного чанка
/// уезжало в вершины другого.
///
/// Не потокобезопасен — вызовы только с главного потока.
class StagingPool {
public:
    bool init(VkDevice dev, VkPhysicalDevice phys);
    void destroy();

    /// Открывает новый пакет: возвращает в оборот то, что GPU уже
    /// дочитал. keep — на сколько пакетов буфер остаётся занятым.
    void collect(u32 keep);

    StagingBuffer* acquire(u64 minSize);
    /// Буфер отдан GPU: освободится через keep пакетов, см. collect().
    void retire(StagingBuffer* s);

    usize count() const { return buffers_.size(); }
    u64 totalBytes() const;

private:
    static u32 findMemoryType(VkPhysicalDevice phys, u32 bits, VkMemoryPropertyFlags p);
    bool resize(StagingBuffer* s, u64 newCap);

    VkDevice         dev_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<StagingBuffer>> buffers_;
    u64 batchNo_ = 0;
    u32 keepBatches_ = 1;
};

} // namespace vk
