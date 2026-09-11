/**
 * @file vk_pipeline.cpp
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#include "vk_pipeline.h"
#include "vk_shader.h"
#include "../core/log.h"

namespace vk {

bool GraphicsPipeline::create(VkDevice dev, ShaderCache& shaders, const PipelineDesc& d) {
    dev_ = dev;
    Shader* vs = shaders.get(d.vertName);
    Shader* fs = shaders.get(d.fragName);
    if (!vs || !fs) {
        LOGE("Pipeline: шейдеры не загружены: %s / %s", d.vertName, d.fragName);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        vs->stage(VK_SHADER_STAGE_VERTEX_BIT),
        fs->stage(VK_SHADER_STAGE_FRAGMENT_BIT),
    };

    // ------------------------------------------------------------
    // Vertex input
    // ------------------------------------------------------------
    // Формат приходит из PipelineDesc: каждый рендерер знает свой.
    constexpr u32 MAX_BINDINGS = 4;
    constexpr u32 MAX_ATTRS    = 12;
    VkVertexInputBindingDescription   vib[MAX_BINDINGS] = {};
    VkVertexInputAttributeDescription via[MAX_ATTRS]    = {};

    const u32 bindCount = d.bindingCount < MAX_BINDINGS ? d.bindingCount : MAX_BINDINGS;
    const u32 attrCount = d.attrCount    < MAX_ATTRS    ? d.attrCount    : MAX_ATTRS;

    for (u32 i = 0; i < bindCount; ++i) {
        vib[i].binding   = i;
        vib[i].stride    = d.bindings[i].stride;
        vib[i].inputRate = d.bindings[i].perInstance
                         ? VK_VERTEX_INPUT_RATE_INSTANCE
                         : VK_VERTEX_INPUT_RATE_VERTEX;
    }
    for (u32 i = 0; i < attrCount; ++i) {
        via[i].location = d.attrs[i].location;
        via[i].binding  = d.attrs[i].binding;
        via[i].format   = d.attrs[i].format;
        via[i].offset   = d.attrs[i].offset;
    }

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount   = bindCount;
    vi.pVertexBindingDescriptions      = vib;
    vi.vertexAttributeDescriptionCount = attrCount;
    vi.pVertexAttributeDescriptions    = via;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates    = dyn;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = d.cullMode;
    rs.frontFace   = d.frontFace;
    rs.lineWidth   = 1.f;
    rs.depthClampEnable        = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable  = d.depthTest;
    dss.depthWriteEnable = d.depthWrite;
    dss.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
    dss.depthBoundsTestEnable = VK_FALSE;
    dss.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = d.blend ? VK_TRUE : VK_FALSE;
    if (d.blend) {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp        = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    // ------------------------------------------------------------
    // Pipeline layout (+ push constants Phase 7)
    // ------------------------------------------------------------
    VkPushConstantRange pcr{};
    pcr.stageFlags = d.pushConstantStage;
    pcr.offset     = 0;
    pcr.size       = d.pushConstantSize;

    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts    = &d.descLayout;
    if (d.pushConstantSize > 0) {
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges    = &pcr;
    }
    if (vkCreatePipelineLayout(dev, &lci, nullptr, &layout_) != VK_SUCCESS) {
        LOGE("PipelineLayout fail");
        return false;
    }

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &dss;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &ds;
    pci.layout              = layout_;
    pci.renderPass          = d.renderPass;
    pci.subpass             = 0;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_) != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines fail: %s/%s", d.vertName, d.fragName);
        vkDestroyPipelineLayout(dev, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
        return false;
    }

    LOGI("Pipeline создан: %s/%s%s",
         d.vertName, d.fragName, d.bindingCount > 1 ? " [instanced]" : "");
    return true;
}

void GraphicsPipeline::destroy() {
    if (pipeline_) vkDestroyPipeline(dev_, pipeline_, nullptr);
    if (layout_)   vkDestroyPipelineLayout(dev_, layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_   = VK_NULL_HANDLE;
}

} // namespace vk
