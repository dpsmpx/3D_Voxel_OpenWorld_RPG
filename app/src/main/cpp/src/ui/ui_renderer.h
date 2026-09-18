/**
 * @file ui_renderer.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
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
    /// Поворот вывода — тот же, что у камеры. Интерфейс считает NDC
    /// сам, на процессоре, поэтому доворачивать его надо здесь.
    void setSurfaceRotation(u32 degrees);

    void beginFrame();
    void pushQuad(glm::vec2 pos, glm::vec2 size,
                  float u0, float v0, float u1, float v1,
                  u32 rgba);
    void pushTexturedQuad(glm::vec2 pos, glm::vec2 size,
                          float u0, float v0, float u1, float v1,
                          u32 rgba) { pushQuad(pos, size, u0, v0, u1, v1, rgba); }
    /// Произвольный треугольник в NDC. Нужен кругам: экранные
    /// кнопки и джойстик из одних прямоугольников не складываются.
    /// Конвейер интерфейса рисует без отсечения, порядок обхода
    /// значения не имеет.
    void pushTri(glm::vec2 a, glm::vec2 b, glm::vec2 c, u32 rgba);
    void endFrame();

    /// Загружает накопленные вершины в GPU и выпускает команды.
    void flush(vk::Context& ctx);

    /// Сколько вершин ушло на GPU в последнем кадре и сколько из них
    /// нарисовано. Интерфейс не видно уже который круг, а по коду он
    /// обязан рисоваться: это единственный способ отличить «не
    /// построился» от «построился, но не виден».
    u32 lastVertices() const { return lastVerts_; }
    u32 lastDrawn()    const { return lastDrawn_; }
    u32 lastDrawCalls() const { return lastDrawCalls_; }

    /// Вершины, накопленные за текущий кадр. Интерфейс целиком
    /// строится на процессоре, так что по этому потоку его можно
    /// проверить без устройства — этим занимается tools/hostcheck.
    const std::vector<UiVertex>& pendingVertices() const { return verts_; }

    float whiteU() const { return whiteU_; }
    float whiteV() const { return whiteV_; }
    /// Раскладка дескрипторов интерфейса. Раньше здесь возвращалась
    /// раскладка поля descSet_, которое init() не заполняет вовсе,
    /// то есть всегда нулевой дескриптор.
    VkDescriptorSetLayout descriptorLayout() const { return descLayout_; }

private:
    VkDevice dev_ = VK_NULL_HANDLE;
    // Буферы интерфейса растут по мере надобности уже во время кадра,
    // а не только при инициализации, поэтому физическое устройство
    // нужно помнить: контекста в ensureCapacity нет.
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;

    // Шейдер и пайплайн
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;

    // Атлас шрифта
    vk::Texture2D fontAtlas_;

    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_   = VK_NULL_HANDLE;
    VkDescriptorSet fontSet_ = VK_NULL_HANDLE;

    /// Буфер вершин на каждый кадр в работе
    struct FrameBuf {
        vk::Buffer vb;
        u32 vertexCount = 0;
        u32 capacity = 0;
    };
    static constexpr u32 MAX_FRAMES = vk::Context::MAX_FRAMES;
    /// По буферу на кадр в работе. Раньше буфер был один на всех:
    /// пока GPU читал вершины прошлого кадра, процессор переписывал
    /// их для следующего — интерфейс мигал и рвался.
    FrameBuf frames_[MAX_FRAMES];

    /// CPU-side буфер
    std::vector<UiVertex> verts_;

    float whiteU_ = 0.f, whiteV_ = 0.f;
    u32 lastVerts_ = 0, lastDrawn_ = 0, lastDrawCalls_ = 0;
    /// cos/sin угла доворота; при нулевом повороте — (1, 0).
    float rotC_ = 1.f, rotS_ = 0.f;
    glm::vec2 rotate(glm::vec2 p) const {
        return { p.x * rotC_ - p.y * rotS_, p.x * rotS_ + p.y * rotC_ };
    }

    bool ensureCapacity(FrameBuf& b, u32 vertsNeeded);
};

} // namespace ui
