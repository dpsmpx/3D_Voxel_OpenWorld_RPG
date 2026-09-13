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

/// Buffer — владеет VkBuffer + VkDeviceMemory.
///
/// Здесь был ещё метод upload(), создававший временный staging и
/// уничтожавший его сразу после записи vkCmdCopyBuffer — то есть до
/// того, как команда выполнится. Его никто не вызывал; удалён, чтобы
/// не вызвал. Для device-local буферов пользуйтесь пакетом передачи
/// vk::Context::beginTransferBatch() и vk::StagingPool.
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

    /// Только для hostVisible; device-local памяти отображения нет.
    void* map();
    void  unmap();

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
