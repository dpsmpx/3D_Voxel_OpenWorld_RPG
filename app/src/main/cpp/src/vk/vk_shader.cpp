/**
 * @file vk_shader.cpp
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#include "vk_shader.h"
#include "../core/log.h"
#include <cstdio>
#include <vector>
#include <memory>
#include <utility>

namespace vk {

bool Shader::loadFromAsset(VkDevice dev, AAssetManager* mgr, const char* path) {
    AAsset* a = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
    if (!a) { LOGE("Shader asset не найден: %s", path); return false; }
    usize size = (usize)AAsset_getLength(a);
    const void* data = AAsset_getBuffer(a);
    bool ok = loadFromMemory(dev, data, size);
    AAsset_close(a);
    if (ok) LOGI("Shader загружен: %s (%zu байт)", path, size);
    return ok;
}

bool Shader::loadFromFile(VkDevice dev, const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { LOGE("Shader file не найден: %s", path); return false; }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<u8> buf((usize)sz);
    std::fread(buf.data(), 1, (usize)sz, f);
    std::fclose(f);
    return loadFromMemory(dev, buf.data(), buf.size());
}

bool Shader::loadFromMemory(VkDevice dev, const void* data, usize size) {
    if (size == 0 || (size % 4) != 0) {
        LOGE("Некорректный размер SPIR-V: %zu", size);
        return false;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode    = (const u32*)data;
    VkResult r = vkCreateShaderModule(dev, &ci, nullptr, &module_);
    if (r != VK_SUCCESS) { LOGE("vkCreateShaderModule: %d", r); return false; }
    return true;
}

void Shader::destroy(VkDevice dev) {
    if (module_) { vkDestroyShaderModule(dev, module_, nullptr); module_ = VK_NULL_HANDLE; }
}

VkPipelineShaderStageCreateInfo Shader::stage(VkShaderStageFlagBits bits) const {
    VkPipelineShaderStageCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage  = bits;
    ci.module = module_;
    ci.pName  = "main";
    return ci;
}

Shader* ShaderCache::get(const char* name) {
    auto it = cache_.find(name);
    if (it != cache_.end()) return it->second.get();
    auto s = std::make_unique<Shader>();
    if (!s->loadFromAsset(dev_, mgr_, name)) return nullptr;
    auto* raw = s.get();
    cache_[name] = std::move(s);
    return raw;
}

void ShaderCache::destroyAll() {
    for (auto& [_, s] : cache_) s->destroy(dev_);
    cache_.clear();
}

} // namespace vk
