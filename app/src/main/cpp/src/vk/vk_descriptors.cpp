/**
 * @file vk_descriptors.cpp
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#include "vk_descriptors.h"
#include "../core/log.h"

#include <vector>

namespace vk {

bool DescriptorSet::create(VkDevice dev, u32 framesInFlight) {
    dev_ = dev;
    frames_ = framesInFlight;

    // --- Layout: 0 = UBO (vertex + fragment) ---
    // Сэмплера здесь больше нет: мир воксельный, текстур у него нет,
    // и единственный оставшийся атлас — шрифтовой — живёт в своём
    // наборе внутри UiRenderer.
    VkDescriptorSetLayoutBinding bindings[1] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layout_) != VK_SUCCESS) return false;

    // --- Pool ---
    VkDescriptorPoolSize sizes[1] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = framesInFlight;

    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = framesInFlight;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &pool_) != VK_SUCCESS) return false;

    // --- Sets ---
    std::vector<VkDescriptorSetLayout> layouts(framesInFlight, layout_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = framesInFlight;
    ai.pSetLayouts = layouts.data();
    sets_.resize(framesInFlight);
    if (vkAllocateDescriptorSets(dev, &ai, sets_.data()) != VK_SUCCESS) return false;

    return true;
}

void DescriptorSet::bindUbo(u32 frame, VkBuffer ubo, u64 size) {
    VkDescriptorBufferInfo bi{};
    bi.buffer = ubo;
    bi.offset = 0;
    bi.range  = size;

    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets_[frame];
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
}


void DescriptorSet::destroy() {
    if (pool_)   vkDestroyDescriptorPool(dev_, pool_, nullptr);
    if (layout_) vkDestroyDescriptorSetLayout(dev_, layout_, nullptr);
    pool_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE; sets_.clear();
}

} // namespace vk
