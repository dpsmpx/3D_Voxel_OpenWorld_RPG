/**
 * @file vk_buffer.h
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#pragma once
#include "../core/types.h"
#include <vulkan/vulkan.h>

namespace vk {

enum class BufferUsage {
    Vertex,
    Index,
    Uniform,
    Staging,
};

/// GpuBuffer — владеет VkBuffer + VkDeviceMemory.
/// Для device-local буферов загрузка идёт через временный staging.
class Buffer {
public:
    bool create(VkDevice dev, VkPhysicalDevice phys,
                u64 size, BufferUsage usage, bool hostVisible);

    void destroy();

    /// Дескрипторы буфера без владельца.
    struct Handles {
        VkDevice       dev = VK_NULL_HANDLE;
        VkBuffer       buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
    };
    /// Отдаёт владение и обнуляет себя. Нужно, чтобы отложить
    /// уничтожение: буфер, из которого GPU ещё читает кадр, нельзя
    /// освобождать в тот момент, когда он перестал быть нужен нам.
    Handles release();

    /// Только для hostVisible. Для device-local — используйте upload().
    void* map();
    void  unmap();

    /// Загрузка данных. Для device-local создаёт staging и копирует.
    /// Для hostVisible делает прямой memcpy.
    void upload(VkDevice dev, VkPhysicalDevice phys, VkCommandBuffer cmdOrNull,
                const void* data, u64 size);

    /// Прямая запись для hostVisible (карты памяти не меняются).
    void write(const void* data, u64 size);

    VkBuffer       handle()      const { return buf_; }
    VkDeviceMemory memory()      const { return mem_; }
    u64            size()        const { return size_; }
    bool           hostVisible() const { return hostVisible_; }

private:
    static u32 findMemoryType(VkPhysicalDevice phys, u32 typeBits, VkMemoryPropertyFlags props);

    VkDevice       dev_ = VK_NULL_HANDLE;
    VkBuffer       buf_ = VK_NULL_HANDLE;
    VkDeviceMemory mem_ = VK_NULL_HANDLE;
    u64            size_ = 0;
    void*          mapped_ = nullptr;
    bool           hostVisible_ = false;
};

} // namespace vk
