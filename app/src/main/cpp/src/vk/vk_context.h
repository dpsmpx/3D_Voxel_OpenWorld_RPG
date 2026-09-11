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

    // ---- Accessors ----
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
    static constexpr u32 MAX_FRAMES = 2;

    // ---- One-shot submit ----
    // Выполняет fn(cmd) в отдельном командном буфере, ждёт завершения.
    // Используется для upload (buffer copy, image layout transition).
    void submitOneShot(const std::function<void(VkCommandBuffer)>& fn);
    void waitIdle() const { if (device_) vkDeviceWaitIdle(device_); }

private:
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
    u32                      imgIdx_ = 0;
    bool                     frameStarted_ = false;
};

} // namespace vk
