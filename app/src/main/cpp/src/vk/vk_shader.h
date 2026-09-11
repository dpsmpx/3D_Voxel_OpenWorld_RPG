/**
 * @file vk_shader.h
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#pragma once
#include "../core/types.h"
#include <vulkan/vulkan.h>
#include <android/asset_manager.h>
#include <string>
#include <memory>
#include <unordered_map>

namespace vk {

/// Один SPIR-V модуль. Шейдеры компилируются glslc на этапе
/// сборки (см. build.sh) и попадают в APK как ассеты
/// `assets/shaders/<имя>.spv`.
class Shader {
public:
    Shader() = default;
    Shader(const Shader&)            = delete;
    Shader& operator=(const Shader&) = delete;

    /// Загружает SPIR-V из ассета APK. path — например "shaders/voxel.vert.spv".
    bool loadFromAsset(VkDevice dev, AAssetManager* mgr, const char* path);

    /// Загружает SPIR-V из файла (отладочный путь, вне APK).
    bool loadFromFile(VkDevice dev, const char* path);

    /// Создаёт VkShaderModule из буфера SPIR-V. size должен быть кратен 4.
    bool loadFromMemory(VkDevice dev, const void* data, usize size);

    void destroy(VkDevice dev);

    /// Описание стадии для VkGraphicsPipelineCreateInfo. Точка входа — "main".
    VkPipelineShaderStageCreateInfo stage(VkShaderStageFlagBits bits) const;

    VkShaderModule module() const { return module_; }
    bool valid() const { return module_ != VK_NULL_HANDLE; }

private:
    VkShaderModule module_ = VK_NULL_HANDLE;
};

/// Кэш шейдеров по имени ассета: один VkShaderModule на файл,
/// сколько бы пайплайнов его ни использовало.
class ShaderCache {
public:
    void init(VkDevice dev, AAssetManager* mgr) { dev_ = dev; mgr_ = mgr; }

    /// Возвращает загруженный шейдер или nullptr, если ассет не найден.
    Shader* get(const char* name);

    void destroyAll();

private:
    VkDevice        dev_ = VK_NULL_HANDLE;
    AAssetManager*  mgr_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Shader>> cache_;
};

} // namespace vk
