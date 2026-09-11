#include "vk_context.h"
#include "../core/log.h"
#include <algorithm>
#include <array>
#include <cstring>

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    LOGE("Vulkan error %d at %s:%d", (int)_r, __FILE__, __LINE__); return false; } } while(0)

namespace vk {

namespace {

VkFormat findDepthFormat(VkPhysicalDevice dev) {
    VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };
    for (auto f : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(dev, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return f;
    }
    return VK_FORMAT_UNDEFINED;
}

u32 findMemoryType(VkPhysicalDevice dev, u32 typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev, &mp);
    for (u32 i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0;
}

} // namespace

bool Context::init(ANativeWindow* window) {
    if (!createInstance()) return false;
    if (!createSurface(window)) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    if (!createDepthResources()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommandPool()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    LOGI("Vulkan: %ux%u, depth=%d", swapExtent_.width, swapExtent_.height, (int)depthFormat_);
    return true;
}

bool Context::createInstance() {
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    const char* exts[]   = { "VK_KHR_surface", "VK_KHR_android_surface" };

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName   = "VoxelRPG";
    app.applicationVersion = VK_MAKE_VERSION(1,0,0);
    app.pEngineName        = "VoxelRPG";
    app.apiVersion         = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = 2;
    ci.ppEnabledExtensionNames = exts;
    // Без слоёв в релизе; включить для отладки
    (void)layers;

    VKCHECK(vkCreateInstance(&ci, nullptr, &instance_));
    return true;
}

bool Context::createSurface(ANativeWindow* w) {
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = w;
    VKCHECK(vkCreateAndroidSurfaceKHR(instance_, &ci, nullptr, &surface_));
    return true;
}

bool Context::pickPhysicalDevice() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (!count) { LOGE("Нет Vulkan-устройств"); return false; }
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance_, &count, devs.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    i32 bestScore = -1;

    for (auto d : devs) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_1) continue;

        // Ищем очередь с графикой и презентацией
        u32 qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, qs.data());

        bool hasGfx = false, hasPresent = false;
        u32 gfxFam = 0, presentFam = 0;
        for (u32 i = 0; i < qCount; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { hasGfx = true; gfxFam = i; }
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if (present) { hasPresent = true; presentFam = i; }
        }
        if (!hasGfx || !hasPresent) continue;

        // Требуем swapchain extension
        u32 extCount = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, exts.data());
        bool hasSwapchain = false;
        for (auto& e : exts) if (!strcmp(e.extensionName, VK_KHR_swapchain)) { hasSwapchain = true; break; }
        if (!hasSwapchain) continue;

        i32 score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        score += props.limits.maxImageDimension2D;

        if (score > bestScore) {
            bestScore = score;
            best = d;
            gfxFamily_ = gfxFam;
        }
    }

    if (best == VK_NULL_HANDLE) { LOGE("Нет подходящего GPU"); return false; }
    physical_ = best;
    return true;
}

bool Context::createLogicalDevice() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gfxFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* exts[] = { "VK_KHR_swapchain" };

    VkPhysicalDeviceFeatures feats{};
    feats.samplerAnisotropy = VK_TRUE;
    feats.fillModeNonSolid  = VK_TRUE;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qci;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = exts;
    ci.pEnabledFeatures        = &feats;

    VKCHECK(vkCreateDevice(physical_, &ci, nullptr, &device_));
    vkGetDeviceQueue(device_, gfxFamily_, 0, &gfxQueue_);
    return true;
}

bool Context::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);

    u32 fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, fmts.data());

    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            { chosen = f; break; }
    }
    swapFormat_ = chosen.format;

    swapExtent_ = caps.currentExtent;
    if (swapExtent_.width == UINT32_MAX) {
        swapExtent_.width  = std::clamp(1080u, caps.minImageExtent.width,  caps.maxImageExtent.width);
        swapExtent_.height = std::clamp(1920u, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    u32 imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface          = surface_;
    ci.minImageCount    = imgCount;
    ci.imageFormat      = chosen.format;
    ci.imageColorSpace  = chosen.colorSpace;
    ci.imageExtent      = swapExtent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    VKCHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));
    vkGetSwapchainImagesKHR(device_, swapchain_, &imgCount, nullptr);
    swapImages_.resize(imgCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imgCount, swapImages_.data());
    return true;
}

bool Context::createImageViews() {
    swapViews_.resize(swapImages_.size());
    for (size_t i = 0; i < swapImages_.size(); ++i) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image    = swapImages_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format   = swapFormat_;
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.layerCount = 1;
        VKCHECK(vkCreateImageView(device_, &ci, nullptr, &swapViews_[i]));
    }
    return true;
}

bool Context::createDepthResources() {
    depthFormat_ = findDepthFormat(physical_);
    if (depthFormat_ == VK_FORMAT_UNDEFINED) { LOGE("Не найден depth format"); return false; }

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = depthFormat_;
    ici.extent = { swapExtent_.width, swapExtent_.height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VKCHECK(vkCreateImage(device_, &ici, nullptr, &depthImage_));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, depthImage_, &req);

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(physical_, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(device_, &ai, nullptr, &depthMem_));
    vkBindImageMemory(device_, depthImage_, depthMem_, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = depthImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = depthFormat_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VKCHECK(vkCreateImageView(device_, &vci, nullptr, &depthView_));
    return true;
}

bool Context::createRenderPass() {
    VkAttachmentDescription color{};
    color.format         = swapFormat_;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format         = depthFormat_;
    depth.samples        = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub{};
    sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount    = 1;
    sub.pColorAttachments       = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { color, depth };

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = (u32)attachments.size();
    ci.pAttachments    = attachments.data();
    ci.subpassCount    = 1;
    ci.pSubpasses      = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VKCHECK(vkCreateRenderPass(device_, &ci, nullptr, &renderPass_));
    return true;
}

bool Context::createFramebuffers() {
    framebuffers_.resize(swapViews_.size());
    for (size_t i = 0; i < swapViews_.size(); ++i) {
        std::array<VkImageView, 2> attachments = { swapViews_[i], depthView_ };
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass      = renderPass_;
        ci.attachmentCount = (u32)attachments.size();
        ci.pAttachments    = attachments.data();
        ci.width           = swapExtent_.width;
        ci.height          = swapExtent_.height;
        ci.layers          = 1;
        VKCHECK(vkCreateFramebuffer(device_, &ci, nullptr, &framebuffers_[i]));
    }
    return true;
}

bool Context::createCommandPool() {
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = gfxFamily_;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKCHECK(vkCreateCommandPool(device_, &ci, nullptr, &cmdPool_));
    return true;
}

bool Context::createCommandBuffers() {
    cmdBuffers_.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = cmdPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (u32)cmdBuffers_.size();
    VKCHECK(vkAllocateCommandBuffers(device_, &ai, cmdBuffers_.data()));
    return true;
}

bool Context::createSyncObjects() {
    imgAvailable_.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinished_.resize(MAX_FRAMES_IN_FLIGHT);
    inFlight_.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VKCHECK(vkCreateSemaphore(device_, &si, nullptr, &imgAvailable_[i]));
        VKCHECK(vkCreateSemaphore(device_, &si, nullptr, &renderFinished_[i]));
        VKCHECK(vkCreateFence    (device_, &fi, nullptr, &inFlight_[i]));
    }
    return true;
}

void Context::destroySwapchain() {
    vkDeviceWaitIdle(device_);
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    vkDestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE;
    vkDestroyImage(device_, depthImage_, nullptr);   depthImage_ = VK_NULL_HANDLE;
    vkFreeMemory(device_, depthMem_, nullptr);       depthMem_ = VK_NULL_HANDLE;
    for (auto v : swapViews_) vkDestroyImageView(device_, v, nullptr);
    swapViews_.clear();
    vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE;
}

void Context::onResize(ANativeWindow* /*window*/) {
    destroySwapchain();
    if (!createSwapchain())    { LOGE("resize: swapchain"); return; }
    if (!createImageViews())   { LOGE("resize: image views"); return; }
    if (!createDepthResources()){ LOGE("resize: depth"); return; }
    if (!createFramebuffers()) { LOGE("resize: framebuffers"); return; }
    LOGI("Swapchain пересоздан: %ux%u", swapExtent_.width, swapExtent_.height);
}

bool Context::beginFrame() {
    vkWaitForFences(device_, 1, &inFlight_[currentFrame_], VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imgAvailable_[currentFrame_], VK_NULL_HANDLE, &imgIdx_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // Пересоздать swapchain — вызовет main loop
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;

    vkResetFences(device_, 1, &inFlight_[currentFrame_]);
    vkResetCommandBuffer(cmdBuffers_[currentFrame_], 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmdBuffers_[currentFrame_], &bi);

    // Открываем render pass
    VkClearValue clears[2];
    clears[0].color = {{ 0.45f, 0.62f, 0.85f, 1.0f }};
    clears[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass        = renderPass_;
    rp.framebuffer       = framebuffers_[imgIdx_];
    rp.renderArea.extent = swapExtent_;
    rp.clearValueCount   = 2;
    rp.pClearValues      = clears;
    vkCmdBeginRenderPass(cmdBuffers_[currentFrame_], &rp, VK_SUBPASS_CONTENTS_INLINE);

    frameStarted_ = true;
    return true;
}

void Context::endFrame() {
    if (!frameStarted_) return;
    vkCmdEndRenderPass(cmdBuffers_[currentFrame_]);
    vkEndCommandBuffer(cmdBuffers_[currentFrame_]);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkSemaphore waitSem[]   = { imgAvailable_[currentFrame_] };
    VkPipelineStageFlags ws[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore sigSem[]    = { renderFinished_[currentFrame_] };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = waitSem;
    si.pWaitDstStageMask = ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuffers_[currentFrame_];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = sigSem;
    vkQueueSubmit(gfxQueue_, 1, &si, inFlight_[currentFrame_]);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = sigSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imgIdx_;
    vkQueuePresentKHR(gfxQueue_, &pi);

    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
    frameStarted_ = false;
}

void Context::shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            vkDestroySemaphore(device_, imgAvailable_[i], nullptr);
            vkDestroySemaphore(device_, renderFinished_[i], nullptr);
            vkDestroyFence(device_, inFlight_[i], nullptr);
        }
        destroySwapchain();
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        vkDestroyCommandPool(device_, cmdPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_)  vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    device_   = VK_NULL_HANDLE;
    surface_  = VK_NULL_HANDLE;
}

void Context::submitOneShot(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = cmdPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &ai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(device_, &fi, nullptr, &fence);

    vkQueueSubmit(gfxQueue_, 1, &si, fence);
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);
}
} // namespace vk