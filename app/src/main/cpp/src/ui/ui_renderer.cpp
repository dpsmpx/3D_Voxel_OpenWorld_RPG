/**
 * @file ui_renderer.cpp
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#include "ui_renderer.h"
#include "ui_atlas.h"
#include "../core/log.h"
#include <cstring>

namespace ui {

// Вершинный формат UI: позиция уже в NDC, поэтому vec2, а не vec3.
static const vk::VertexBinding kBindings[1] = { { sizeof(UiVertex), false } };
static const vk::VertexAttr kAttrs[3] = {
    { 0, 0, VK_FORMAT_R32G32_SFLOAT,  0  },   // inPos
    { 1, 0, VK_FORMAT_R32G32_SFLOAT,  8  },   // inUv
    { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16 },   // inColor
};

bool UiRenderer::init(vk::Context& ctx, AAssetManager* mgr) {
    dev_ = ctx.device();
    phys_ = ctx.physicalDevice();
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

    // Набор ровно один — атлас шрифта. Второй заводился под внешний
    // атлас, которым рисовалась миникарта.
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
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
    pd.bindings     = kBindings;
    pd.bindingCount = 1;
    pd.attrs        = kAttrs;
    pd.attrCount    = 3;
    if (!pipeline_.create(dev_, shaders_, pd)) return false;

    verts_.reserve(8192);
    LOGI("UiRenderer готов");
    return true;
}

void UiRenderer::setSurfaceRotation(u32 degrees) {
    // Тот же угол и тот же знак, что в Camera::projection — знак там
    // выверен устройством. x' = x*cos - y*sin, y' = x*sin + y*cos.
    switch (degrees) {
        case 90:  rotC_ =  0.f; rotS_ =  1.f; break;   // +90
        case 180: rotC_ = -1.f; rotS_ =  0.f; break;
        case 270: rotC_ =  0.f; rotS_ = -1.f; break;   // -90
        default:  rotC_ =  1.f; rotS_ =  0.f; break;
    }
}

void UiRenderer::beginFrame() {
    verts_.clear();
}
void UiRenderer::endFrame() {}

void UiRenderer::pushQuad(glm::vec2 pos, glm::vec2 size,
                          float u0, float v0, float u1, float v1, u32 rgba)
{
    u8 r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF;
    u8 b = (rgba >>  8) & 0xFF, a = (rgba      ) & 0xFF;
    auto& V = verts_;
    const glm::vec2 p1 = pos + size;

    // Ровно шесть вершин: рисуем списком треугольников без индексов.
    // Раньше сюда клались ещё и четыре угла «для себя», и на квад
    // выходило десять вершин. Десять не делится на три: треугольники
    // собирались из вершин разных прямоугольников, весь поток
    // разъезжался, и интерфейса на экране не было.
    const UiVertex c0{ rotate({pos.x, pos.y}), {u0, v0}, r, g, b, a };
    const UiVertex c1{ rotate({p1.x,  pos.y}), {u1, v0}, r, g, b, a };
    const UiVertex c2{ rotate({p1.x,  p1.y}),  {u1, v1}, r, g, b, a };
    const UiVertex c3{ rotate({pos.x, p1.y}),  {u0, v1}, r, g, b, a };
    V.push_back(c0); V.push_back(c1); V.push_back(c2);
    V.push_back(c0); V.push_back(c2); V.push_back(c3);
}

void UiRenderer::pushTri(glm::vec2 a, glm::vec2 b, glm::vec2 c, u32 rgba) {
    u8 r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF;
    u8 bl = (rgba >>  8) & 0xFF, al = (rgba      ) & 0xFF;
    auto& V = verts_;
    // -1 — признак сплошной заливки, см. shaders/ui.frag.
    const glm::vec2 uv{ -1.f, -1.f };
    V.push_back({ rotate(a), uv, r, g, bl, al });
    V.push_back({ rotate(b), uv, r, g, bl, al });
    V.push_back({ rotate(c), uv, r, g, bl, al });
}

bool UiRenderer::ensureCapacity(FrameBuf& b, u32 vertsNeeded) {
    if (b.capacity >= vertsNeeded && b.vb.handle()) return true;
    if (b.vb.handle()) b.vb.destroy();
    u32 cap = b.capacity ? b.capacity : 4096;
    while (cap < vertsNeeded) cap *= 2;
    u64 bytes = (u64)cap * sizeof(UiVertex);
    if (!b.vb.create(dev_, phys_, bytes, vk::BufferUsage::Vertex, true)) {
        LOGE("интерфейс: не создан вершинный буфер на %llu байт",
             (unsigned long long)bytes);
        return false;
    }
    b.capacity = cap;
    return true;
}

void UiRenderer::flush(vk::Context& ctx) {
    VkCommandBuffer cmd = ctx.currentCmd();

    ctx.setFullViewport(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());

    lastVerts_ = (u32)verts_.size();
    lastDrawn_ = 0;
    lastDrawCalls_ = 0;

    // Ругаемся один раз: пустой интерфейс — это не «нечего показать»,
    // это ошибка, и отличить её от «нарисовали, но не видно» иначе
    // нельзя.
    static bool warnedEmpty = false;
    if (lastVerts_ == 0 && !warnedEmpty) {
        warnedEmpty = true;
        LOGW("интерфейс: за кадр не построено ни одной вершины");
    }

    if (verts_.empty()) return;

    const u32 frame = ctx.frameInFlight() % MAX_FRAMES;
    FrameBuf& fb = frames_[frame];
    if (!ensureCapacity(fb, (u32)verts_.size())) return;

    void* m = fb.vb.map();
    if (!m) return;
    std::memcpy(m, verts_.data(), verts_.size() * sizeof(UiVertex));
    // Отображение НЕ снимаем. Буфер отображается при создании и
    // остаётся таким на всю жизнь — так задумано, и так работают
    // все остальные буферы, доступные процессору. Здесь стоял
    // unmap(), и следующий map() возвращал nullptr: кадр молча
    // пропускался, и интерфейс жил ровно два первых кадра за весь
    // запуск. По журналу это выглядело как «вершин 3468,
    // нарисовано 0».
    fb.vertexCount = (u32)verts_.size();

    if (fontSet_ == VK_NULL_HANDLE) return;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &fontSet_, 0, nullptr);
    VkDeviceSize offs[] = { 0 };
    const VkBuffer vb = fb.vb.handle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offs);
    vkCmdDraw(cmd, fb.vertexCount, 1, 0, 0);
    lastDrawn_ += fb.vertexCount;
    ++lastDrawCalls_;
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
    fontSet_ = VK_NULL_HANDLE;
}

} // namespace ui
