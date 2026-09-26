/**
 * @file mesh_table.h
 * @brief Рендер: меши воксельных моделей в общем буфере — таблица и вызовы отрисовки.
 *
 * Общее у всех, кто рисует воксельные модели экземплярами: предметы и
 * растения в игровом кадре, растения на изометрическом снимке.
 * Загрузка — двумя путями: через vk::Context (игра: буферы в
 * видеопамяти через промежуточный) и напрямую в память, видимую
 * процессору (снимок: у него свой жизненный цикл на голых ручках
 * Vulkan, и контекста игрового кадра он не знает). Первый путь
 * определён рядом с другими загрузками игрового рендера
 * (voxel_model_renderer.cpp), остальное — в mesh_table.cpp: так
 * инструменты без контекста (tools/isocheck) собирают таблицу без него.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_buffer.h"
#include "voxel_model.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace vk { class Context; }

namespace render {

/// Меши моделей в одном буфере вершин и одном буфере индексов.
class MeshTable {
public:
    /// Загрузить меши. Номер меша — индекс в массиве.
    bool set(vk::Context& ctx, const std::vector<VoxelMesh>& meshes);
    /// То же без vk::Context: буферы видимы процессору и пишутся
    /// напрямую. Для рендеров со своей целью и своим жизненным циклом
    /// (изометрический снимок): им нужны устройство и память, а не
    /// игровой кадр с его очередью разовых команд.
    bool setHostVisible(VkDevice dev, VkPhysicalDevice phys, const std::vector<VoxelMesh>& meshes);
    void destroy();

    u32  count() const { return (u32)ranges_.size(); }
    bool valid() const { return vbo_.handle() && ibo_.handle(); }
    /// Нечего рисовать: номера нет или меш пуст.
    bool empty(u32 i) const { return i >= ranges_.size() || ranges_[i].indexCount == 0; }
    u32  quads(u32 i) const { return empty(i) ? 0u : ranges_[i].indexCount / 6u; }

    /// Привязать буфер вершин (привязка 0) вместе с буфером экземпляров
    /// (привязка 1) и буфер индексов.
    void bind(VkCommandBuffer cmd, VkBuffer instances) const;
    void draw(VkCommandBuffer cmd, u32 mesh, u32 instanceCount, u32 firstInstance) const;

private:
    vk::Buffer vbo_;
    vk::Buffer ibo_;
    struct Range { u32 firstIndex = 0, indexCount = 0; i32 vertexOffset = 0; };
    std::vector<Range> ranges_;

    /// Склеить меши в общие массивы и разметить диапазоны.
    void gather(const std::vector<VoxelMesh>& meshes, std::vector<VoxelModelVertex>& verts,
                std::vector<u32>& idx);
};

} // namespace render
