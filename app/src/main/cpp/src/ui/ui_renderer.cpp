#include "ui_renderer.h"
#include "ui_atlas.h"
#include "../core/log.h"
#include <cstring>

namespace ui {

bool UiRenderer::init(vk::Context& ctx, AAssetManager* mgr) {
    dev_ = ctx.device();
    shaders_.init(dev_, mgr);

    UiAtlasData a = buildUiAtlas();
    if (!fontAtlas_.create(ctx.device(), ctx.physicalDevice(),
                           ctx.gfxQueue(), ctx.gfxFamily(),
                           a.width, a.height, VK_FORMAT_R8G8B8A8_UNORM,
                           a.pixels.data(), a.pixels.size(),
                           VK_FILTER_NEAREST,
                           VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                           /*mips=*/false)) {
        LOGE("UI atlas не создан");
        return false;
    }
    whiteU_ = a.whiteU;
    whiteV_ = a.whiteV;

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev_, &lci, nullptr, &descLayout_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 2;

    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev_, &pci, nullptr, &descPool_) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &descLayout_;
    if (vkAllocateDescriptorSets(dev_, &ai, &fontSet_) != VK_SUCCESS) return false;

    VkDescriptorImageInfo ii{};
    ii.imageView   = fontAtlas_.view();
    ii.sampler     = fontAtlas_.sampler();
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = fontSet_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);

    vk::PipelineDesc pd{};
    pd.renderPass  = ctx.renderPass();
    pd.descLayout  = descLayout_;
    pd.vertName    = "shaders/ui.vert.spv";
    pd.fragName    = "shaders/ui.frag.spv";
    pd.depthFormat = ctx.depthFormat();
    pd.cullMode    = VK_CULL_MODE_NONE;
    pd.depthTest   = false;
    pd.depthWrite  = false;
    pd.blend       = true;
    if (!pipeline_.create(dev_, shaders_, pd)) return false;

    verts_[0].reserve(8192);
    verts_[1].reserve(2048);
    LOGI("UiRenderer готов");
    return true;
}

void UiRenderer::attachExternalAtlas(VkImageView view, VkSampler sampler) {
    if (extSet_ != VK_NULL_HANDLE) return;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &descLayout_;
    if (vkAllocateDescriptorSets(dev_, &ai, &extSet_) != VK_SUCCESS) {
        LOGE("UiRenderer: не удалось аллоцировать external atlas set");
        return;
    }
    VkDescriptorImageInfo ii{};
    ii.imageView   = view;
    ii.sampler     = sampler;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = extSet_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    LOGI("UiRenderer: external atlas подключён");
}

void UiRenderer::beginFrame() {
    verts_[0].clear();
    verts_[1].clear();
    activeSlot_ = 0;
}
void UiRenderer::setAtlas(int slot) { activeSlot_ = (slot == 1) ? 1 : 0; }
void UiRenderer::endFrame() {}

void UiRenderer::pushQuad(glm::vec2 pos, glm::vec2 size,
                          float u0, float v0, float u1, float v1, u32 rgba)
{
    u8 r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF;
    u8 b = (rgba >>  8) & 0xFF, a = (rgba      ) & 0xFF;
    auto& V = verts_[activeSlot_];
    glm::vec2 p1 = pos + size;
    V.push_back({ {pos.x, pos.y}, {u0, v0}, r, g, b, a });
    V.push_back({ {p1.x,  pos.y}, {u1, v0}, r, g, b, a });
    V.push_back({ {p1.x,  p1.y}, {u1, v1}, r, g, b, a });
    V.push_back({ {pos.x, p1.y}, {u0, v1}, r, g, b, a });
    u32 base = (u32)V.size() - 4;
    V.push_back(V[base + 0]); V.push_back(V[base + 2]); V.push_back(V[base + 3]);
    V.push_back(V[base + 0]); V.push_back(V[base + 1]); V.push_back(V[base + 2]);
}

bool UiRenderer::ensureCapacity(FrameBuf& b, u32 vertsNeeded) {
    if (b.capacity >= vertsNeeded && b.vb.handle()) return true;
    if (b.vb.handle()) b.vb.destroy();
    u32 cap = b.capacity ? b.capacity : 4096;
    while (cap < vertsNeeded) cap *= 2;
    u64 bytes = (u64)cap * sizeof(UiVertex);
    if (!b.vb.create(dev_, VK_NULL_HANDLE, bytes, vk::BufferUsage::Vertex, true)) return false;
    b.capacity = cap;
    return true;
}

void UiRenderer::flush(vk::Context& ctx) {
    VkCommandBuffer cmd = ctx.currentCmd();

    VkViewport vp{};
    vp.width  = (f32)ctx.extent().width;
    vp.height = (f32)ctx.extent().height;
    vp.minDepth = 0.f; vp.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{}; sc.extent = ctx.extent();
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());

    for (int slot = 0; slot < 2; ++slot) {
        auto& V = verts_[slot];
        if (V.empty()) continue;

        FrameBuf& fb = frames_[slot];
        if (!ensureCapacity(fb, (u32)V.size())) continue;

        void* m = fb.vb.map();
        if (!m) continue;
        std::memcpy(m, V.data(), V.size() * sizeof(UiVertex));
        fb.vb.unmap();
        fb.vertexCount = (u32)V.size();

        VkDescriptorSet ds = (slot == 0) ? fontSet_ : extSet_;
        if (ds == VK_NULL_HANDLE) continue;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_.layout(), 0, 1, &ds, 0, nullptr);
        VkDeviceSize offs[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, &fb.vb.handle(), offs);
        vkCmdDraw(cmd, fb.vertexCount, 1, 0, 0);
    }
}

void UiRenderer::destroy() {
    for (auto& f : frames_) f.vb.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    fontAtlas_.destroy();
    if (descPool_)   vkDestroyDescriptorPool(dev_, descPool_, nullptr);
    if (descLayout_) vkDestroyDescriptorSetLayout(dev_, descLayout_, nullptr);
    descPool_ = VK_NULL_HANDLE;
    descLayout_ = VK_NULL_HANDLE;
    fontSet_ = extSet_ = VK_NULL_HANDLE;
}

} // namespace ui