/**
 * @file grass_pipeline.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "grass_pipeline.h"
#include "instanced_renderer.h"

namespace render {

// Вершинный формат: перекрещенные трапеции + инстанс GrassInstance.
static const vk::VertexBinding kGrassBindings[2] = {
    { sizeof(GrassVertex),   false },   // vec3 pos
    { sizeof(GrassInstance), true  },
};
static const vk::VertexAttr kGrassAttrs[5] = {
    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // inPos
    { 1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // iPos
    { 2, 1, VK_FORMAT_R32_SFLOAT,       12 },   // iScale
    { 3, 1, VK_FORMAT_R8G8B8A8_UNORM,   16 },   // iColor
    { 4, 1, VK_FORMAT_R32_SFLOAT,       20 },   // iYaw
};

namespace {
// Пучок травы: две перекрещённые трапеции, широкие у земли и узкие
// кверху. Текстуры нет, поэтому форму должна задавать геометрия:
// прямоугольник без текстуры читается как торчащий из земли лист
// бумаги, а сужающийся клин — как трава.
// Пучок шириной в целый блок и без текстуры читается не как трава, а
// как сплошной зелёный конус — именно это и было видно на устройстве.
// Лист должен быть узким: тогда две перекрещённые трапеции читаются
// как пучок листьев, а не как закрашенная пирамида.
constexpr f32 BASE = 0.17f;   // полуширина у земли
constexpr f32 TIP  = 0.03f;   // полуширина у верхушки

const GrassVertex CROSS_VERTS[8] = {
    { {-BASE, 0.f,  0.f  } },   // 1: низ слева
    { { BASE, 0.f,  0.f  } },   // 1: низ справа
    { { TIP,  1.f,  0.f  } },   // 1: верх справа
    { {-TIP,  1.f,  0.f  } },   // 1: верх слева
    { { 0.f,  0.f, -BASE } },   // 2: низ слева
    { { 0.f,  0.f,  BASE } },   // 2: низ справа
    { { 0.f,  1.f,  TIP  } },   // 2: верх справа
    { { 0.f,  1.f, -TIP  } },   // 2: верх слева
};

const u32 CROSS_INDICES[12] = {
    0, 1, 2, 0, 2, 3,     // трапеция 1
    4, 5, 6, 4, 6, 7,     // трапеция 2
};
} // namespace

const GrassVertex* grassVerts(u32& count) {
    count = (u32)(sizeof(CROSS_VERTS) / sizeof(CROSS_VERTS[0]));
    return CROSS_VERTS;
}

const u32* grassIndices(u32& count) {
    count = (u32)(sizeof(CROSS_INDICES) / sizeof(CROSS_INDICES[0]));
    return CROSS_INDICES;
}

vk::PipelineDesc grassPipelineDesc(VkRenderPass rp, VkDescriptorSetLayout layout,
                                   VkFormat depthFormat)
{
    vk::PipelineDesc d{};
    d.renderPass  = rp;
    d.descLayout  = layout;
    d.vertName    = "shaders/grass.vert.spv";
    d.fragName    = "shaders/grass.frag.spv";
    d.depthFormat = depthFormat;
    d.cullMode    = VK_CULL_MODE_NONE;   // биллборд — рисуем с обеих сторон
    d.depthTest   = true;
    d.depthWrite  = true;
    // Текстуры с альфой больше нет, значит и смешивание не нужно:
    // трава пишет глубину как обычная геометрия, и её не приходится
    // сортировать — заодно пропал целый класс артефактов порядка.
    d.blend       = false;
    d.bindings     = kGrassBindings;
    d.bindingCount = 2;
    d.attrs        = kGrassAttrs;
    d.attrCount    = 5;
    return d;
}

} // namespace render
