/**
 * @file vk_renderpass.cpp
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 *
 * Отдельный модуль, потому что проход рендера нужен и офлайн-проверке
 * графики (tools/vkcheck), а vk_context.cpp тянет за собой окно
 * Android и цепочку показа — на хосте их нет.
 */
#include "vk_context.h"
#include "../core/log.h"
#include <array>

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    LOGE("Vulkan error %d at %s:%d", (int)_r, __FILE__, __LINE__); return false; } } while(0)

namespace vk {

bool createVoxelRenderPass(VkDevice dev, VkFormat color_, VkFormat depth_,
                           VkImageLayout finalColorLayout, VkRenderPass* out) {
    VkAttachmentDescription color{};
    color.format         = color_;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = finalColorLayout;

    VkAttachmentDescription depth{};
    depth.format         = depth_;
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

    // Зависимость от всего, что отправлено в очередь раньше.
    //
    // Буфер глубины в движке ОДИН на всю цепочку показа, а кадров в
    // работе два: пока показывается один, процессор уже записывает
    // команды следующего, и оба пишут в одну и ту же глубину. Прежняя
    // зависимость этого не закрывала:
    //
    //   * srcStageMask не включал LATE_FRAGMENT_TESTS. Глубина
    //     пишется на двух стадиях — ранней и поздней, — и ждать
    //     только раннюю значит не ждать ничего: прошлый кадр
    //     дописывает глубину поздней стадией уже после того, как
    //     следующий её очистил;
    //
    //   * srcAccessMask был нулевым, то есть барьер вовсе не делал
    //     прошлые записи доступными. Порядок «запись после записи»
    //     требует и выполнения, и памяти; без второго кэш вложения
    //     мог не дойти до памяти к моменту очистки;
    //
    //   * dstAccessMask не упоминал чтения, а тест глубины именно
    //     читает.
    //
    // Итог на экране — ровно то, на что жаловались: глубина одного
    // кадра проверялась против остатков другого, и сквозь ближние
    // блоки просвечивало то, что за ними.
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { color, depth };

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = (u32)attachments.size();
    ci.pAttachments    = attachments.data();
    ci.subpassCount    = 1;
    ci.pSubpasses      = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VKCHECK(vkCreateRenderPass(dev, &ci, nullptr, out));
    return true;
}

} // namespace vk
