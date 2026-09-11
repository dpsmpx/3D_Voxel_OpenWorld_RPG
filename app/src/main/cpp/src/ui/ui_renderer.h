/**
 * @file ui_renderer.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню, миникарта.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_texture.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_descriptors.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace ui {

/// Формат UI-вершины: pos(NDC) + uv + цвет
struct UiVertex {
    glm::vec2 pos;      // 8 байт
    glm::vec2 uv;       // 8 байт
    u8 r, g, b, a;      // 4 байта
};                       // = 20

class UiRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr);
    void destroy();

    /// Один "батч" = один descriptor set (атлас). Мы держим 2 батча:
    ///   slot 0 — font atlas (по умолчанию)
    ///   slot 1 — block atlas (external)
    void beginFrame();
    void setAtlas(int slot);   // 0 или 1
    void pushQuad(glm::vec2 pos, glm::vec2 size,
                  float u0, float v0, float u1, float v1,
                  u32 rgba);
    void pushTexturedQuad(glm::vec2 pos, glm::vec2 size,
                          float u0, float v0, float u1, float v1,
                          u32 rgba) { pushQuad(pos, size, u0, v0, u1, v1, rgba); }
    void endFrame();

    /// Загружает накопленные вершины в GPU и выпускает команды.
    void flush(vk::Context& ctx);

    /// Второй атлас (block atlas) — подключается извне
    void attachExternalAtlas(VkImageView view, VkSampler sampler);

    float whiteU() const { return whiteU_; }
    float whiteV() const { return whiteV_; }
    VkDescriptorSetLayout descriptorLayout() const { return descSet_.layout(); }

private:
    VkDevice dev_ = VK_NULL_HANDLE;

    // Шейдер и пайплайн
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    vk::DescriptorSet    descSet_;      // layout = 1 sampler2D

    // Атлас шрифта
    vk::Texture2D fontAtlas_;

    /// Внешний (block) атлас — descriptor set для него
    VkDescriptorSet extSet_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_   = VK_NULL_HANDLE;
    VkDescriptorSet fontSet_ = VK_NULL_HANDLE;

    /// Буферы вершин (по 2 на кадр in flight)
    struct FrameBuf {
        vk::Buffer vb;
        u32 vertexCount = 0;
        u32 capacity = 0;
    };
    static constexpr u32 MAX_FRAMES = vk::Context::MAX_FRAMES;
    FrameBuf frames_[MAX_FRAMES];

    /// CPU-side буферы
    std::vector<UiVertex> verts_[2];  // по атласу
    int activeSlot_ = 0;
    u32 currentFrame_ = 0;

    float whiteU_ = 0.f, whiteV_ = 0.f;

    bool ensureCapacity(FrameBuf& b, u32 vertsNeeded);
};

} // namespace ui
