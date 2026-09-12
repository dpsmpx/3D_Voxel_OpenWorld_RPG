/**
 * @file instance_ring.h
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_context.h"
#include <cstring>

namespace render {

/// Кольцо буферов инстансов: по одному на каждый кадр, который может
/// быть в работе, плюс один про запас.
///
/// Раньше каждый инстансный рендерер на каждом кадре создавал
/// временный буфер, копировал через него данные отдельной отправкой в
/// очередь и ЖДАЛ её завершения. Рендереров пять — мобы, NPC,
/// предметы, снаряды, трава, — то есть пять выделений памяти и пять
/// полных остановок GPU за кадр. Выделений памяти в Vulkan к тому же
/// ограниченное число, обычно несколько тысяч на процесс.
///
/// Инстансов здесь сотни, а не миллионы: несколько килобайт на кадр.
/// Писать их прямо в память, видимую процессору, и дешевле, и проще —
/// ни промежуточного буфера, ни копии, ни отправки, ни ожидания.
///
/// Кольцо нужно затем, что кадр пишется до того, как GPU дорисовал
/// предыдущие. Буфер, в который пишем сейчас, последний раз читался
/// три кадра назад — столько их в работе не бывает.
class InstanceRing {
public:
    static constexpr u32 SLOTS = vk::Context::MAX_FRAMES + 1;

    void init(VkDevice dev, VkPhysicalDevice phys) { dev_ = dev; phys_ = phys; }

    /// Готовит буфер под следующий кадр и записывает в него данные.
    /// Возвращает false, если буфер не создался — рисовать нечего.
    bool write(const void* data, u64 bytes) {
        if (bytes == 0 || dev_ == VK_NULL_HANDLE) return false;
        slot_ = (slot_ + 1) % SLOTS;
        auto& b = buf_[slot_];
        if (!b.handle() || b.size() < bytes) {
            if (b.handle()) b.destroy();
            // С запасом: число мобов пляшет от кадра к кадру, и
            // пересоздавать буфер на каждую новую особь незачем.
            const u64 cap = bytes + bytes / 2 + 1024;
            if (!b.create(dev_, phys_, cap, vk::BufferUsage::Vertex, true))
                return false;
        }
        b.write(data, bytes);
        return true;
    }

    VkBuffer handle() const { return buf_[slot_].handle(); }

    void destroy() {
        for (auto& b : buf_) b.destroy();
        slot_ = 0;
    }

private:
    VkDevice         dev_  = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    vk::Buffer       buf_[SLOTS];
    u32              slot_ = 0;
};

} // namespace render
