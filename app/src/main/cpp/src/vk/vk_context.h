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

/// Проход рендера мира — одно описание на игру и на проверку.
///
/// Вынесено из Context затем, что офлайн-проверка графики
/// (tools/vkcheck) обязана строить ТОТ ЖЕ проход: зависимости
/// подпрохода, операции загрузки и раскладки вложений — это ровно то
/// место, где ошибка видна только на устройстве. Пока числа жили
/// внутри Context, проверить их было нечем: Context требует окна
/// Android и цепочки показа, а проверка идёт без экрана.
///
/// finalColorLayout — единственное, чем два случая отличаются: игра
/// отдаёт изображение показу, проверка — копированию.
bool createVoxelRenderPass(VkDevice dev, VkFormat color, VkFormat depth,
                           VkImageLayout finalColorLayout, VkRenderPass* out);

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

    /// Сколько миллисекунд GPU РИСОВАЛ последний измеренный кадр.
    ///
    /// Без этого числа журнал с устройства не отвечает на главный
    /// вопрос. Время «рисование» в сводке меряется вокруг
    /// beginFrame/endFrame, а внутри beginFrame стоит ожидание забора
    /// и vkAcquireNextImageKHR — то есть ожидание вертикальной
    /// синхронизации. В одну величину слиты «GPU занят» и «мы ждём
    /// экран», а лечатся они противоположным.
    ///
    /// Считается метками времени самого GPU вокруг командного буфера
    /// кадра. Ноль означает, что устройство меток не умеет.
    f32              lastGpuMs() const { return lastGpuMs_; }
    bool             gpuTimingAvailable() const { return timestampPeriod_ > 0.f; }
    /// Последняя ошибка vkQueuePresentKHR (VK_SUCCESS, если её не было).
    VkResult         lastPresentResult() const { return lastPresent_; }
    /// Сколько раз пересоздавалась цепочка показа. Здоровое число —
    /// единицы за сеанс: по одному на поворот экрана. Если оно растёт
    /// вместе с кадрами, значит ответ показа снова принимают за приказ
    /// пересоздавать, и половина кадров не доходит до экрана.
    u64              swapchainRebuilds() const { return swapchainRebuilds_; }
    static constexpr u32 MAX_FRAMES = 2;

    /// Пакет передачи больше не ждёт GPU на процессоре, поэтому его
    /// командный буфер нельзя ни освободить, ни перезаписать сразу.
    /// Кольцо из нескольких слотов: к моменту, когда очередь снова
    /// доходит до слота, его работа давно закончена. Столько же
    /// пакетов держатся занятыми staging-буферы, см. vk::StagingPool.
    static constexpr u32 TRANSFER_SLOTS = MAX_FRAMES + 1;

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
    /// Семафоры и отметки занятости, привязанные к изображениям цепочки.
    bool createSwapchainSync();
    void destroySwapchainSync();
    void destroySwapchain();
    /// Изменился ли размер окна с момента создания цепочки.
    bool surfaceExtentChanged() const;
    /// Разбирает кадр, который захватил изображение, но не дошёл до
    /// показа: подаёт забор слота и забирает семафор изображения.
    void discardAcquiredFrame();

    VkInstance       instance_ = VK_NULL_HANDLE;
    /// Обработчик сообщений слоя проверки. Живёт, только если слой
    /// нашёлся на устройстве; иначе VK_NULL_HANDLE и ноль накладных.
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
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

    /// По одному на кадр в работе: ими распоряжается сам кадр.
    std::vector<VkSemaphore> imgAvailable_;
    std::vector<VkFence>     inFlight_;
    /// Обещал ли кто-нибудь подать забор слота. Отправка в очередь
    /// может быть отвергнута, и тогда забор не подадут никогда —
    /// а ожидание на нём бессрочное.
    bool                     framePending_[MAX_FRAMES] = {};

    /// А эти — ПО ОДНОМУ НА ИЗОБРАЖЕНИЕ ЦЕПОЧКИ, и это принципиально.
    ///
    /// renderFinished_ ждёт vkQueuePresentKHR, а показ асинхронный: он
    /// может быть ещё не выполнен, когда очередь кадров снова дойдёт до
    /// того же слота. Раньше семафор выбирался по номеру кадра в
    /// работе, и тогда мы подавали сигнал на семафор, которого кто-то
    /// ещё ждёт. Драйвер вправе перепутать, какой сигнал чей: на экран
    /// попадает недорисованное изображение, а то и два кадра разом.
    ///
    /// imagesInFlight_ — чей забор сейчас держит это изображение.
    /// Забор кадра говорит только про слот кадра; про то, свободна ли
    /// картинка, которую вернул vkAcquireNextImageKHR, он не знает
    /// ничего, а вернуть он может любую.
    ///
    /// Оба массива живут ровно столько же, сколько сама цепочка:
    /// число изображений при пересоздании может измениться.
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkFence>     imagesInFlight_;   ///< чужие заборы, не наши
    u32                      currentFrame_ = 0;
    u64                      framesPresented_ = 0;

    // ---- Метки времени GPU ----
    // Пул на две метки (начало и конец) для каждого кадра в работе.
    // Читаем результат не сразу, а когда кадр гарантированно
    // завершился, — иначе vkGetQueryPoolResults либо заблокирует
    // процессор, либо вернёт «ещё не готово».
    VkQueryPool              timeQuery_ = VK_NULL_HANDLE;
    f32                      timestampPeriod_ = 0.f;   ///< нс на такт; 0 — меток нет
    bool                     timeQueryPending_[MAX_FRAMES] = {};
    f32                      lastGpuMs_ = 0.f;
    u64                      swapchainRebuilds_ = 0;
    VkResult                 lastPresent_ = VK_SUCCESS;
    bool                     needsResize_ = false;
    /// Поворот экрана, каким его видит драйвер (в отличие от того,
    /// какой мы просим через preTransform).
    VkSurfaceTransformFlagBitsKHR displayTransform_ =
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    /// Мы сами попросили preTransform, не совпадающий с поворотом
    /// экрана, — значит ответ SUBOPTIMAL на показе ожидаем и не
    /// является поводом пересоздавать цепочку.
    bool                     presentMayBeSuboptimal_ = false;
    u32                      suboptimalPresents_ = 0;
    /// Как часто перепроверять размер окна, пока показ отвечает
    /// SUBOPTIMAL: примерно раз в секунду при 60 кадрах.
    static constexpr u32     SUBOPTIMAL_RECHECK = 60;
    u32                      imgIdx_ = 0;
    bool                     frameStarted_ = false;

    /// Пакет передач: буфер и забор переиспользуются между кадрами.
    struct TransferSlot {
        VkCommandBuffer cmd   = VK_NULL_HANDLE;
        VkFence         fence = VK_NULL_HANDLE;
        bool            submitted = false;
    };
    TransferSlot             transfers_[TRANSFER_SLOTS];
    u32                      transferSlot_    = 0;
    u64                      transferBatchNo_ = 0;
    VkCommandBuffer          transferCmd_     = VK_NULL_HANDLE;
};

} // namespace vk
