/**
 * @file vk_context.cpp
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#include "vk_context.h"
#include "../core/log.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <vector>

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

/// Есть ли слой среди тех, что предлагает загрузчик.
bool instanceLayerPresent(const char* name) {
    u32 n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS || n == 0)
        return false;
    std::vector<VkLayerProperties> ls(n);
    if (vkEnumerateInstanceLayerProperties(&n, ls.data()) != VK_SUCCESS)
        return false;
    for (const auto& l : ls)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

bool instanceExtensionPresent(const char* name) {
    u32 n = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS || n == 0)
        return false;
    std::vector<VkExtensionProperties> es(n);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, es.data()) != VK_SUCCESS)
        return false;
    for (const auto& e : es)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

/// Сообщение слоя проверки уходит в тот же журнал, что и всё
/// остальное. Это важнее, чем кажется: logcat чужого приложения на
/// телефоне без отладчика недоступен, а файл читается из Termux.
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT /*types*/,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* /*user*/)
{
    const char* id  = (data && data->pMessageIdName) ? data->pMessageIdName : "?";
    const char* msg = (data && data->pMessage)       ? data->pMessage       : "";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        LOGE("[vk] %s: %s", id, msg);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        LOGW("[vk] %s: %s", id, msg);
    else
        LOGI("[vk] %s: %s", id, msg);
    // VK_FALSE: вызов, на который пожаловались, всё равно выполняется.
    // Слой здесь — наблюдатель, а не цензор.
    return VK_FALSE;
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
    LOGI("Vulkan: %ux%u, depth=%d", swapExtent_.width, swapExtent_.height,
         (int)depthFormat_);
    return true;
}

// Слой проверки включается САМ, если он есть на устройстве.
//
// Все ошибки этого движка, найденные за последние круги, — перепутанный
// семафор показа, запись в буфер под читающим его GPU, неполная
// зависимость подпрохода по глубине — слой называет вслух и сразу же.
// Искать их по картинке на экране стоило дней.
//
// На обычном телефоне слоя нет: vkEnumerateInstanceLayerProperties
// возвращает пустой список, и всё происходит ровно как раньше, без
// единого лишнего вызова. Чтобы он появился, достаточно положить
// libVkLayer_khronos_validation.so в jniLibs/arm64-v8a (он есть в NDK,
// в toolchains/llvm/.../lib/linux/arm64) и пересобрать APK. Поэтому
// проверка «есть ли слой» и есть выключатель: отдельного флага
// сборки не нужно, а забыть его переключить невозможно.
bool Context::createInstance() {
    std::vector<const char*> exts   = { "VK_KHR_surface", "VK_KHR_android_surface" };
    std::vector<const char*> layers;

    const bool haveValidation = instanceLayerPresent("VK_LAYER_KHRONOS_validation");
    const bool haveDebugUtils = instanceExtensionPresent(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (haveValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        if (haveDebugUtils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        LOGI("Vulkan: слой проверки найден и включён%s",
             haveDebugUtils ? " (сообщения пойдут в этот журнал)"
                            : " (VK_EXT_debug_utils нет — сообщения только в logcat)");
    } else {
        LOGI("Vulkan: слоя проверки на устройстве нет — работаем без него");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName   = "VoxelRPG";
    app.applicationVersion = VK_MAKE_VERSION(1,0,0);
    app.pEngineName        = "VoxelRPG";
    app.apiVersion         = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = (u32)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount       = (u32)layers.size();
    ci.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();

    // Ошибки самого vkCreateInstance слой сообщить не успеет: его ещё
    // нет. Цепляем обработчик прямо к созданию через pNext.
    VkDebugUtilsMessengerCreateInfoEXT dbg{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;
    if (haveValidation && haveDebugUtils) ci.pNext = &dbg;

    VKCHECK(vkCreateInstance(&ci, nullptr, &instance_));

    if (haveValidation && haveDebugUtils) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance_, "vkCreateDebugUtilsMessengerEXT");
        if (create && create(instance_, &dbg, nullptr, &debugMessenger_) != VK_SUCCESS) {
            debugMessenger_ = VK_NULL_HANDLE;
            LOGW("Vulkan: обработчик сообщений слоя не создан");
        }
    }
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

        // Нужно одно семейство, умеющее и графику, и презентацию:
        // движок использует одну очередь, раздельные семейства
        // потребовали бы передачи владения изображениями swapchain.
        bool hasUniversal = false;
        u32  gfxFam = 0;
        for (u32 i = 0; i < qCount; ++i) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if (!present) continue;
            hasUniversal = true;
            gfxFam = i;
            break;
        }
        if (!hasUniversal) continue;

        // Требуем swapchain extension
        u32 extCount = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, exts.data());
        bool hasSwapchain = false;
        for (auto& e : exts) if (!strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { hasSwapchain = true; break; }
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

// Режим показа по настройке «ограничивать частоту кадров».
//
// FIFO есть всегда — это единственный режим, который спецификация
// обязывает поддерживать. Остальные надо спрашивать: без снятия
// ограничения максимальную частоту не увидеть, а увидеть её нужно,
// чтобы отличить «упёрлись в GPU» от «ждём экран».
//
// Без ограничения берём IMMEDIATE: он показывает ровно то, что машина
// успевает, ценой разрыва кадра. MAILBOX разрывов не даёт, но он же и
// не даёт увидеть максимум — лишние кадры он выбрасывает, а на экран
// отдаёт всё те же шестьдесят в секунду. Поэтому он только запасной,
// на устройствах без IMMEDIATE.
VkPresentModeKHR Context::choosePresentMode() {
    presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    if (vsyncWanted_) return presentMode_;

    u32 n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    if (n) vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());

    auto has = [&](VkPresentModeKHR m) {
        for (auto x : modes) if (x == m) return true;
        return false;
    };
    if      (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) presentMode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (has(VK_PRESENT_MODE_MAILBOX_KHR))   presentMode_ = VK_PRESENT_MODE_MAILBOX_KHR;
    else {
        LOGW("Без ограничения кадров просили, но устройство даёт только FIFO");
    }
    return presentMode_;
}

void Context::setVsync(bool on) {
    if (vsyncWanted_ == on) return;
    vsyncWanted_ = on;
    // Режим показа — свойство цепочки. Просим пересоздать её тем же
    // путём, которым это делает поворот экрана: beginFrame вернёт
    // false, и главный цикл позовёт onResize.
    needsResize_ = true;
    LOGI("Ограничение кадров по экрану: %s (цепочка будет пересоздана)",
         on ? "включено" : "снято");
}

bool Context::createSwapchain() {
    // Ответ обязательно проверяем: при неудаче caps остаётся мусором
    // из стека, и из него получаются и размер цепочки, и число
    // изображений, и preTransform.
    VkSurfaceCapabilitiesKHR caps{};
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps));

    u32 fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, fmts.data());
    // Пустой список означает, что поверхность уже недействительна
    // (окно свернули между проверкой и запросом). fmts[0] на пустом
    // векторе — чтение за границей, и падает оно далеко от причины.
    if (fmts.empty()) { LOGE("Поверхность не предлагает ни одного формата"); return false; }

    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            { chosen = f; break; }
    }
    swapFormat_ = chosen.format;

    // Поворот экрана.
    //
    // Раньше здесь стояло preTransform = currentTransform — обещание
    // композитору, что содержимое уже повёрнуто нами. Обещание никто
    // не выполнял, и мир лежал на боку. Попытка выполнить его,
    // довернув проекцию, дала мир вверх ногами: знак поворота в
    // координатах отсечения противоположен тому, что кажется
    // очевидным, а размеры буфера здесь приходят уже в ориентации
    // ОКНА — то есть переворачивать их не надо вовсе.
    //
    // Просим IDENTITY: композитор доворачивает сам, и ни проекции, ни
    // интерфейсу знать о повороте не нужно. Это стоит одного
    // полноэкранного прохода композиции — на современных телефонах его
    // делает аппаратный композитор, то есть почти даром, — зато
    // снимает целый класс ошибок, на который уже ушло два круга.
    //
    // Если устройство IDENTITY не поддерживает, доворачиваем сами; в
    // этом случае surfaceRotationDegrees() вернёт ненулевой угол.
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        surfaceTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
        surfaceTransform_ = caps.currentTransform;
    }
    displayTransform_ = caps.currentTransform;
    const VkSurfaceTransformFlagBitsKHR displayTransform = caps.currentTransform;

    // Осознанное расхождение: мы просим одно, экран живёт в другом.
    // Драйвер обязан отвечать на каждый показ VK_SUBOPTIMAL_KHR — это
    // не поломка, а буквальный ответ «можно было бы и лучше». Пока мы
    // помним, что расхождение наше собственное, этот ответ надо
    // молча пропускать: пересоздание цепочки его не лечит, потому что
    // после пересоздания расхождение ровно то же.
    presentMayBeSuboptimal_ = (surfaceTransform_ != caps.currentTransform);
    suboptimalPresents_ = 0;

    swapExtent_ = caps.currentExtent;
    // Свёрнутое окно: цепочка нулевого размера не создаётся, а
    // ошибка vkCreateSwapchainKHR об этом не скажет ничего внятного.
    if (swapExtent_.width == 0 || swapExtent_.height == 0) {
        LOGW("Свопчейн: окно нулевого размера, цепочка не создаётся");
        return false;
    }
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
    ci.preTransform     = surfaceTransform_;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = choosePresentMode();
    ci.clipped          = VK_TRUE;

    auto transformName = [](VkSurfaceTransformFlagBitsKHR t) {
        switch (t) {
            case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:  return "90";
            case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "180";
            case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "270";
            case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:   return "нет";
            default: return "иной";
        }
    };
    auto presentName = [](VkPresentModeKHR m) {
        switch (m) {
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE (без ограничения)";
            case VK_PRESENT_MODE_MAILBOX_KHR:   return "MAILBOX (без разрывов)";
            case VK_PRESENT_MODE_FIFO_KHR:      return "FIFO (по экрану)";
            default: return "иной";
        }
    };
    LOGI("Свопчейн: %ux%u, экран повёрнут на %s, просим preTransform %s, показ %s",
         swapExtent_.width, swapExtent_.height,
         transformName(displayTransform), transformName(surfaceTransform_),
         presentName(ci.presentMode));

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
    return createVoxelRenderPass(device_, swapFormat_, depthFormat_,
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, &renderPass_);
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
    cmdBuffers_.resize(MAX_FRAMES);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = cmdPool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (u32)cmdBuffers_.size();
    VKCHECK(vkAllocateCommandBuffers(device_, &ai, cmdBuffers_.data()));
    return true;
}

bool Context::createSyncObjects() {
    imgAvailable_.resize(MAX_FRAMES);
    inFlight_.resize(MAX_FRAMES);

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (u32 i = 0; i < MAX_FRAMES; ++i) {
        VKCHECK(vkCreateSemaphore(device_, &si, nullptr, &imgAvailable_[i]));
        VKCHECK(vkCreateFence    (device_, &fi, nullptr, &inFlight_[i]));
    }

    // Метки времени GPU: две на кадр в работе.
    //
    // Нужны затем, что время «рисование» в сводке меряется вокруг
    // beginFrame/endFrame, а внутри beginFrame стоит ожидание забора и
    // vkAcquireNextImageKHR — то есть ожидание экрана. В одно число
    // слиты «GPU занят» и «мы ждём вертикальную синхронизацию», а
    // лечится это противоположным. Метки отвечают на вопрос прямо.
    //
    // Устройство может их не уметь (timestampPeriod == 0 или нулевая
    // разрядность у семейства очередей) — тогда просто живём без них.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical_, &props);

        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qs.data());
        const bool queueHasBits =
            gfxFamily_ < qn && qs[gfxFamily_].timestampValidBits > 0;

        if (props.limits.timestampPeriod > 0.f && queueHasBits) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = MAX_FRAMES * 2;
            if (vkCreateQueryPool(device_, &qi, nullptr, &timeQuery_) == VK_SUCCESS) {
                timestampPeriod_ = props.limits.timestampPeriod;
                LOGI("Метки времени GPU: есть (%.1f нс на такт)",
                     (double)timestampPeriod_);
            } else {
                timeQuery_ = VK_NULL_HANDLE;
            }
        } else {
            LOGI("Метки времени GPU: устройство их не даёт");
        }
    }

    return createSwapchainSync();
}

// Семафор «кадр дорисован» обязан принадлежать ИЗОБРАЖЕНИЮ, а не
// слоту кадра: его ждёт показ, а показ асинхронный и может быть ещё
// не выполнен, когда очередь кадров вернётся к тому же слоту.
bool Context::createSwapchainSync() {
    destroySwapchainSync();

    renderFinished_.resize(swapImages_.size());
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& sem : renderFinished_)
        VKCHECK(vkCreateSemaphore(device_, &si, nullptr, &sem));

    // Заборы здесь чужие — те же, что в inFlight_. Мы их не создаём и
    // не уничтожаем, только запоминаем, кто держит какое изображение.
    imagesInFlight_.assign(swapImages_.size(), VK_NULL_HANDLE);
    return true;
}

void Context::destroySwapchainSync() {
    for (auto sem : renderFinished_)
        if (sem != VK_NULL_HANDLE) vkDestroySemaphore(device_, sem, nullptr);
    renderFinished_.clear();
    imagesInFlight_.clear();
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

// Дешёвая проверка «окно действительно стало другим». Один запрос к
// драйверу; вызывается только когда показ пожаловался.
bool Context::surfaceExtentChanged() const {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS)
        return false;
    if (caps.currentExtent.width == UINT32_MAX) return false;   // размер задаём мы
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
    return caps.currentExtent.width  != swapExtent_.width ||
           caps.currentExtent.height != swapExtent_.height;
}

void Context::onResize(ANativeWindow* /*window*/) {
    destroySwapchain();
    if (!createSwapchain())    { LOGE("resize: swapchain"); return; }
    // Число изображений при пересоздании может измениться, а семафоры
    // «дорисовано» привязаны именно к ним.
    if (!createSwapchainSync()){ LOGE("resize: sync"); return; }
    if (!createImageViews())   { LOGE("resize: image views"); return; }
    if (!createDepthResources()){ LOGE("resize: depth"); return; }
    if (!createFramebuffers()) { LOGE("resize: framebuffers"); return; }
    ++swapchainRebuilds_;
    LOGI("Swapchain пересоздан (%llu-й раз): %ux%u",
         (unsigned long long)swapchainRebuilds_,
         swapExtent_.width, swapExtent_.height);
}

bool Context::beginFrame() {
    // Показ предыдущего кадра сообщил, что swapchain устарел. Раньше
    // этот ответ игнорировался: кадры продолжали уходить в устаревшую
    // цепочку, и на экране не менялось ничего.
    if (needsResize_) {
        needsResize_ = false;
        return false;
    }

    // Пересоздание цепочки могло не удаться — тогда рисовать не во
    // что. vkAcquireNextImageKHR нулевую цепочку не проверяет, а
    // разыменовывает: процесс падает внутри libvulkan, где от нашего
    // кода не остаётся ни имени функции, ни строки.
    if (swapchain_ == VK_NULL_HANDLE || framebuffers_.empty()) return false;
    if (imgAvailable_[currentFrame_] == VK_NULL_HANDLE) {
        // Слот остался без семафора после отвергнутой отправки —
        // пробуем вернуть его в строй, а не выбывать навсегда.
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(device_, &si, nullptr,
                              &imgAvailable_[currentFrame_]) != VK_SUCCESS) {
            imgAvailable_[currentFrame_] = VK_NULL_HANDLE;
            return false;
        }
    }

    // Ждём забор, только если кто-то обещал его подать. Без этой
    // проверки слот, чью отправку очередь отвергла, вставал бы здесь
    // навсегда: забор так и остался неподанным, а ждём мы без срока.
    if (framePending_[currentFrame_])
        vkWaitForFences(device_, 1, &inFlight_[currentFrame_], VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imgAvailable_[currentFrame_], VK_NULL_HANDLE, &imgIdx_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // Пересоздать swapchain — вызовет main loop
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;

    // Пересоздание цепочки могло оборваться на полпути — тогда кадровый
    // буфер для этой картинки есть в списке, но нулевой. Нулевой
    // дескриптор драйвер не проверяет, а разыменовывает.
    if (imgIdx_ >= framebuffers_.size() || framebuffers_[imgIdx_] == VK_NULL_HANDLE) {
        LOGE("beginFrame: нет кадрового буфера для изображения %u из %zu",
             imgIdx_, framebuffers_.size());
        discardAcquiredFrame();
        needsResize_ = true;
        return false;
    }

    // Забор слота кадра сказал только, что освободился СЛОТ. Какое
    // изображение вернёт vkAcquireNextImageKHR — заранее неизвестно, и
    // оно может быть занято другим кадром, ещё не дорисованным.
    // Изображений в цепочке обычно три, кадров в работе два: рано или
    // поздно очередь выдаёт картинку, в которую ещё пишут.
    if (imgIdx_ < imagesInFlight_.size() &&
        imagesInFlight_[imgIdx_] != VK_NULL_HANDLE)
    {
        vkWaitForFences(device_, 1, &imagesInFlight_[imgIdx_], VK_TRUE, UINT64_MAX);
    }
    if (imgIdx_ < imagesInFlight_.size())
        imagesInFlight_[imgIdx_] = inFlight_[currentFrame_];

    vkResetFences(device_, 1, &inFlight_[currentFrame_]);
    framePending_[currentFrame_] = false;
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
    // Метка начала — до прохода рендера, чтобы в измеренное попало всё,
    // что кадр отдаёт GPU.
    if (timeQuery_ != VK_NULL_HANDLE) {
        // Результат прошлого круга этого слота уже готов: выше мы
        // дождались его забора. Читаем ДО того, как сбросим метки.
        if (timeQueryPending_[currentFrame_]) {
            u64 stamps[2] = { 0, 0 };
            const VkResult qr = vkGetQueryPoolResults(
                device_, timeQuery_, currentFrame_ * 2, 2,
                sizeof(stamps), stamps, sizeof(u64), VK_QUERY_RESULT_64_BIT);
            if (qr == VK_SUCCESS && stamps[1] > stamps[0]) {
                lastGpuMs_ = (f32)((double)(stamps[1] - stamps[0]) *
                                   (double)timestampPeriod_ * 1e-6);
            }
            timeQueryPending_[currentFrame_] = false;
        }
        vkCmdResetQueryPool(cmdBuffers_[currentFrame_], timeQuery_,
                            currentFrame_ * 2, 2);
        vkCmdWriteTimestamp(cmdBuffers_[currentFrame_],
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            timeQuery_, currentFrame_ * 2);
    }

    vkCmdBeginRenderPass(cmdBuffers_[currentFrame_], &rp, VK_SUBPASS_CONTENTS_INLINE);

    frameStarted_ = true;
    return true;
}

// Кадр обрывается, не дойдя до показа.
//
// После него остаются две мины. Забор слота никто не подаст — и
// следующий заход в этот слот встанет на бессрочном ожидании: телефон
// выглядит зависшим, а в журнале ни строки. Семафор изображения,
// наоборот, уже подан захватом, и ждать его теперь некому — следующий
// захват подал бы сигнал на уже поданный семафор.
//
// Пустая отправка снимает обе разом: она забирает семафор и подаёт
// забор. Если очередь не принимает даже её, остаётся дождаться
// простоя устройства и снять отметки честно: забор считаем
// неподанным, изображение — свободным, а семафор создаём заново.
void Context::discardAcquiredFrame() {
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores    = &imgAvailable_[currentFrame_];
    si.pWaitDstStageMask  = &stage;
    if (vkQueueSubmit(gfxQueue_, 1, &si, inFlight_[currentFrame_]) == VK_SUCCESS) {
        framePending_[currentFrame_] = true;
        return;
    }

    vkDeviceWaitIdle(device_);
    framePending_[currentFrame_] = false;
    if (imgIdx_ < imagesInFlight_.size())
        imagesInFlight_[imgIdx_] = VK_NULL_HANDLE;
    vkDestroySemaphore(device_, imgAvailable_[currentFrame_], nullptr);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device_, &sci, nullptr,
                          &imgAvailable_[currentFrame_]) != VK_SUCCESS) {
        imgAvailable_[currentFrame_] = VK_NULL_HANDLE;
        LOGE("discardAcquiredFrame: семафор изображения не пересоздан");
    }
}

void Context::endFrame() {
    if (!frameStarted_) return;
    // Семафоры привязаны к изображениям цепочки; если пересоздание
    // цепочки не довело дело до конца, лучше потерять кадр, чем выйти
    // за границы массива.
    if (imgIdx_ >= renderFinished_.size()) {
        LOGE("endFrame: нет семафора для изображения %u из %zu",
             imgIdx_, renderFinished_.size());
        vkCmdEndRenderPass(cmdBuffers_[currentFrame_]);
        vkEndCommandBuffer(cmdBuffers_[currentFrame_]);
        discardAcquiredFrame();
        frameStarted_ = false;
        needsResize_ = true;
        return;
    }
    vkCmdEndRenderPass(cmdBuffers_[currentFrame_]);
    if (timeQuery_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmdBuffers_[currentFrame_],
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            timeQuery_, currentFrame_ * 2 + 1);
        timeQueryPending_[currentFrame_] = true;
    }
    vkEndCommandBuffer(cmdBuffers_[currentFrame_]);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkSemaphore waitSem[]   = { imgAvailable_[currentFrame_] };
    VkPipelineStageFlags ws[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    // Семафор «дорисовано» берём по ИЗОБРАЖЕНИЮ, а не по слоту кадра:
    // его ждёт показ, а показ может быть ещё не выполнен, когда слот
    // пойдёт на второй круг. Подавать сигнал на семафор, которого
    // кто-то ещё ждёт, нельзя — драйвер вправе перепутать, чей это
    // сигнал, и показать недорисованное.
    VkSemaphore sigSem[]    = { renderFinished_[imgIdx_] };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = waitSem;
    si.pWaitDstStageMask = ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuffers_[currentFrame_];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = sigSem;
    const VkResult sub = vkQueueSubmit(gfxQueue_, 1, &si, inFlight_[currentFrame_]);
    if (sub != VK_SUCCESS) {
        // Семафор «дорисовано» тоже не будет подан, а показ его
        // ждёт: показывать после отвергнутой отправки нельзя вовсе.
        // Остальное — забор слота и семафор изображения — разбирает
        // discardAcquiredFrame().
        LOGE("vkQueueSubmit: %d — кадр потерян", (int)sub);
        discardAcquiredFrame();
        lastPresent_  = sub;
        needsResize_  = true;
        currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES;
        frameStarted_ = false;
        return;
    }

    framePending_[currentFrame_] = true;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = sigSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imgIdx_;
    const VkResult pres = vkQueuePresentKHR(gfxQueue_, &pi);
    lastPresent_ = pres;
    if (pres == VK_ERROR_OUT_OF_DATE_KHR) {
        needsResize_ = true;
    } else if (pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR) {
        // «Suboptimal» — это не «устарело»: кадр показан. Пока мы сами
        // просим preTransform, отличный от поворота экрана, драйвер
        // обязан возвращать этот ответ на КАЖДОМ кадре. Прежний код
        // принимал его за приказ пересоздать цепочку — и цепочка
        // пересоздавалась шестьдесят раз в секунду, каждый раз с
        // vkDeviceWaitIdle и с полностью пропущенным следующим кадром.
        // Мир при этом не успевал нарисоваться ни разу.
        if (pres == VK_SUBOPTIMAL_KHR) {
            if (!presentMayBeSuboptimal_) {
                // Расхождения мы не просили — значит оно настоящее.
                needsResize_ = true;
            } else {
                // Поворот мы отдали композитору осознанно, а вот
                // изменившийся размер окна — настоящая причина
                // пересоздать. Спрашиваем драйвер редко: ответ
                // SUBOPTIMAL приходит каждый кадр, а окно так часто
                // не меняется.
                if (suboptimalPresents_ == 0)
                    LOGI("показ отвечает SUBOPTIMAL — это ожидаемо: поворот "
                         "отдан композитору; следим только за размером окна");
                if ((suboptimalPresents_++ % SUBOPTIMAL_RECHECK) == 0 &&
                    surfaceExtentChanged())
                    needsResize_ = true;
            }
        }
        // Первый показанный кадр — важная веха: до него «чёрный экран»
        // и «кадры не доходят до экрана» выглядят одинаково.
        if (framesPresented_ == 0) LOGI("первый кадр показан на экране");
        ++framesPresented_;
    } else {
        // Об ошибке показа надо знать: без неё «чёрный экран» неотличим
        // от «кадры рисуются, но не доходят».
        static VkResult reported = VK_SUCCESS;
        if (pres != reported) {
            reported = pres;
            LOGE("vkQueuePresentKHR: %d", (int)pres);
        }
    }

    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES;
    frameStarted_ = false;
}

void Context::shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        transferCmd_ = VK_NULL_HANDLE;
        for (auto& slot : transfers_) {
            if (slot.cmd != VK_NULL_HANDLE)
                vkFreeCommandBuffers(device_, cmdPool_, 1, &slot.cmd);
            if (slot.fence != VK_NULL_HANDLE)
                vkDestroyFence(device_, slot.fence, nullptr);
            slot = TransferSlot{};
        }
        destroySwapchainSync();
        if (timeQuery_ != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device_, timeQuery_, nullptr);
            timeQuery_ = VK_NULL_HANDLE;
        }
        for (u32 i = 0; i < MAX_FRAMES; ++i) {
            vkDestroySemaphore(device_, imgAvailable_[i], nullptr);
            vkDestroyFence(device_, inFlight_[i], nullptr);
        }
        destroySwapchain();
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        vkDestroyCommandPool(device_, cmdPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_)  vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (debugMessenger_ != VK_NULL_HANDLE) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    device_   = VK_NULL_HANDLE;
    surface_  = VK_NULL_HANDLE;
}

// ============================================================
// Пакетная передача: один командный буфер и один забор на весь
// кадр загрузки. Заменяет схему «submit + wait на каждую копию»,
// из-за которой загрузка чанка стоила восьми остановок GPU.
// ============================================================
VkCommandBuffer Context::beginTransferBatch() {
    if (transferCmd_ != VK_NULL_HANDLE) {
        LOGE("beginTransferBatch: пакет уже открыт");
        return VK_NULL_HANDLE;
    }

    TransferSlot& slot = transfers_[transferSlot_];

    if (slot.fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device_, &fi, nullptr, &slot.fence) != VK_SUCCESS) {
            LOGE("beginTransferBatch: не создан fence");
            return VK_NULL_HANDLE;
        }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool        = cmdPool_;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &ai, &slot.cmd) != VK_SUCCESS) {
            slot.cmd = VK_NULL_HANDLE;
            LOGE("beginTransferBatch: не выделен командный буфер");
            return VK_NULL_HANDLE;
        }
    } else if (slot.submitted) {
        // Этот слот отдавали GPU TRANSFER_SLOTS пакетов назад — к
        // этому моменту он давно закончен, и ожидание ничего не стоит.
        // Оно нужно только чтобы переиспользовать командный буфер.
        vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        vkResetCommandBuffer(slot.cmd, 0);
    }
    vkResetFences(device_, 1, &slot.fence);
    slot.submitted = false;

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.cmd, &bi);

    // Записи идут в буферы, из которых ещё может читать предыдущий
    // кадр. Область действия барьера — вся очередь: первая половина
    // захватывает всё, что отправлено раньше, поэтому копии дождутся
    // чужого чтения вершин без остановки процессора.
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(slot.cmd,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &before, 0, nullptr, 0, nullptr);

    transferCmd_ = slot.cmd;
    return transferCmd_;
}

void Context::endTransferBatch() {
    if (transferCmd_ == VK_NULL_HANDLE) return;
    TransferSlot& slot = transfers_[transferSlot_];

    // Вторая половина барьера: дальше вершинный ввод читает всё, что
    // мы только что скопировали. Она тоже действует на всю очередь,
    // то есть и на кадр, который будет отправлен следом.
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(transferCmd_,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 1, &after, 0, nullptr, 0, nullptr);

    vkEndCommandBuffer(transferCmd_);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &transferCmd_;

    // Раньше здесь стояло ожидание на заборе — процессор простаивал,
    // пока GPU не доделает и копии, и всё, что стояло в очереди до
    // них, то есть весь предыдущий кадр. На загрузке чанков это
    // съедало пятнадцать миллисекунд из шестнадцати. Порядок теперь
    // держат барьеры очереди, а забор нужен лишь для того, чтобы
    // через TRANSFER_SLOTS пакетов переиспользовать командный буфер.
    const VkResult r = vkQueueSubmit(gfxQueue_, 1, &si, slot.fence);
    if (r != VK_SUCCESS) LOGE("endTransferBatch: vkQueueSubmit %d", (int)r);
    slot.submitted = (r == VK_SUCCESS);

    transferCmd_  = VK_NULL_HANDLE;
    transferSlot_ = (transferSlot_ + 1) % TRANSFER_SLOTS;
    ++transferBatchNo_;
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
