/**
 * @file vk_context.h
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#pragma once
#include "../core/types.h"
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <functional>
#include <vector>

namespace vk {

class Context {
public:
    bool init(ANativeWindow* window);
    void shutdown();
    void onResize(ANativeWindow* window);

    bool beginFrame();
    void endFrame();

    /// ---- Accessors ----
    VkDevice         device()        const { return device_; }
    VkPhysicalDevice physicalDevice()const { return physical_; }
    VkRenderPass     renderPass()    const { return renderPass_; }
    VkCommandBuffer  currentCmd()    const { return cmdBuffers_[currentFrame_]; }
    VkExtent2D       extent()        const { return swapExtent_; }
    VkFormat         colorFormat()   const { return swapFormat_; }
    VkFormat         depthFormat()   const { return depthFormat_; }
    VkQueue          gfxQueue()      const { return gfxQueue_; }
    u32              gfxFamily()     const { return gfxFamily_; }
    u32              imageIndex()    const { return imgIdx_; }
    u32              frameInFlight() const { return currentFrame_; }

    /// Сколько кадров реально ушло на экран. Ноль при работающем цикле
    /// означает, что показывать нечего или показ отвергается.
    u64              framesPresented() const { return framesPresented_; }
    /// Последняя ошибка vkQueuePresentKHR (VK_SUCCESS, если её не было).
    VkResult         lastPresentResult() const { return lastPresent_; }
    static constexpr u32 MAX_FRAMES = 2;

    // ---- Одиночная передача ----
    /// Выполняет fn(cmd) в отдельном командном буфере и ждёт завершения.
    /// Полная остановка конвейера, поэтому годится только для редких
    /// операций на старте (layout-переходы текстур). Для потоковой
    /// загрузки чанков используйте пакет beginTransferBatch/end.
    void submitOneShot(const std::function<void(VkCommandBuffer)>& fn);

    // ---- Пакетная передача ----
    /// Открывает командный буфер, куда можно сложить произвольное
    /// число копий. Возвращает VK_NULL_HANDLE при ошибке.
    /// Вложенные вызовы запрещены.
    VkCommandBuffer beginTransferBatch();

    /// Отправляет накопленный пакет и ждёт его завершения — один
    /// vkQueueSubmit и одно ожидание на любое количество копий.
    void endTransferBatch();

    bool transferBatchOpen() const { return transferCmd_ != VK_NULL_HANDLE; }

    /// Насколько композитор повернёт наш кадр при выводе, в градусах.
    /// Мы обещали ему это через preTransform, значит обязаны повернуть
    /// содержимое сами — в проекции камеры и в интерфейсе.
    u32 surfaceRotationDegrees() const {
        switch (surfaceTransform_) {
            case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:  return 90;
            case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return 180;
            case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return 270;
            default: return 0;
        }
    }
    /// Меняются ли местами ширина и высота при выводе.
    bool surfaceSwapsAxes() const {
        const u32 d = surfaceRotationDegrees();
        return d == 90 || d == 270;
    }

    void waitIdle() const { if (device_) vkDeviceWaitIdle(device_); }

private:
    VkSurfaceTransformFlagBitsKHR surfaceTransform_ =
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

    bool createInstance();
    bool createSurface(ANativeWindow* w);
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain();
    bool createImageViews();
    bool createDepthResources();
    bool createRenderPass();
    bool createFramebuffers();
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    void destroySwapchain();

    VkInstance       instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_  = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice         device_   = VK_NULL_HANDLE;
    VkQueue          gfxQueue_ = VK_NULL_HANDLE;
    u32              gfxFamily_ = 0;

    VkSwapchainKHR           swapchain_ = VK_NULL_HANDLE;
    VkFormat                 swapFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D               swapExtent_{};
    std::vector<VkImage>     swapImages_;
    std::vector<VkImageView> swapViews_;

    VkFormat       depthFormat_ = VK_FORMAT_UNDEFINED;
    VkImage        depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMem_   = VK_NULL_HANDLE;
    VkImageView    depthView_  = VK_NULL_HANDLE;

    VkRenderPass               renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool                cmdPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBuffers_;

    std::vector<VkSemaphore> imgAvailable_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkFence>     inFlight_;
    u32                      currentFrame_ = 0;
    u64                      framesPresented_ = 0;
    VkResult                 lastPresent_ = VK_SUCCESS;
    bool                     needsResize_ = false;
    u32                      imgIdx_ = 0;
    bool                     frameStarted_ = false;

    /// Пакет передач: буфер и забор переиспользуются между кадрами.
    VkCommandBuffer          transferCmd_   = VK_NULL_HANDLE;
    VkFence                  transferFence_ = VK_NULL_HANDLE;
};

} // namespace vk
