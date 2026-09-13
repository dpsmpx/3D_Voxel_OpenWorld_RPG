/**
 * @file minimap.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "../vk/vk_texture.h"
#include "../world/chunk_manager.h"
#include <glm/glm.hpp>
#include <vector>

namespace ui {

class Minimap {
public:
    bool init(vk::Context& ctx, u32 px = 128);
    void destroy();

    /// Продвигает перерисовку карты. Вызывать каждый кадр: проход
    /// разбит на порции строк, потому что целиком он стоит около
    /// дюжины миллисекунд — целого кадра. Новый проход начинается
    /// сам, когда игрок заметно сдвинулся или снимок устарел.
    void update(world::ChunkManager& world,
                const glm::vec3& playerPos,
                f32 radiusBlocks,
                f32 dt);

    /// Phase 15: если текстура обновилась — заливаем в GPU.
    /// Вызывать из render/prepareFrame.
    void flushUpload(vk::Context& ctx);

    VkImageView view()    const { return tex_.view(); }
    VkSampler   sampler() const { return tex_.sampler(); }
    u32         size()    const { return size_; }

    f32 blocksPerPixel() const { return blocksPerPixel_; }
    const glm::vec3& center() const { return center_; }

    bool dirty() const { return dirty_; }

private:
    VkDevice          dev_ = VK_NULL_HANDLE;
    /// Собственный пул команд для заливки текстуры: у vk::Context
    /// публичного пула нет, а создавать свой на каждую заливку —
    /// лишняя работа драйверу раз в секунду.
    VkCommandPool     pool_ = VK_NULL_HANDLE;
    vk::Texture2D     tex_;
    std::vector<u8>   pixels_;
    u32               size_ = 128;
    f32               blocksPerPixel_ = 1.f;
    glm::vec3         center_{0.f};
    bool              dirty_ = false;

    /// Незаконченный снимок. Меняется с pixels_ только целиком:
    /// показывать половину карты от старого положения игрока, а
    /// половину от нового — хуже, чем показывать чуть устаревшую.
    std::vector<u8>   scratch_;
    u32               rowCursor_  = 0;
    bool              passActive_ = false;
    bool              everDrawn_  = false;
    glm::vec3         passCenter_{ 1e9f };   ///< заведомо «далеко»: первый проход обязателен
    f32               passScale_  = 1.f;
    f32               sincePass_  = 0.f;

    /// Сколько строк рисуем за вызов: 128 строк проходят примерно за
    /// шестнадцать кадров, то есть за четверть секунды.
    static constexpr u32 ROWS_PER_CALL   = 8;
    /// Насколько игрок должен сдвинуться, чтобы карту стоило
    /// перерисовать, и через сколько секунд обновить её всё равно.
    static constexpr f32 MOVE_THRESHOLD  = 2.f;
    static constexpr f32 REFRESH_SECONDS = 5.f;
};

/// Цвет блока на миникарте в формате RGBA8.
/// @param blockId идентификатор блока
/// @return упакованный цвет; для неизвестных блоков — серый
u32 blockMapColor(u16 blockId);

} // namespace ui
