/**
 * @file voxel_model.h
 * @brief Рендер: воксельные модели из мелких вокселей — меш и значок.
 */
#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>
#include <vector>

namespace render {

/// Модель из мелких вокселей своего цвета.
///
/// Мир собран из кубов, и всё, что в нём лежит, обязано быть из того
/// же материала: выпавший меч, нарисованный цветным кубиком, читался
/// как «что-то редкое», а не как меч. Модель — это сетка вокселей
/// размером с ладонь (до 16 на сторону), у каждого свой цвет.
///
/// Оси: X вправо, Y вверх, Z к зрителю. Модель стоит на Y = 0 —
/// так она и ложится на землю.
struct VoxelModel {
    i32 sx = 0, sy = 0, sz = 0;
    /// 0xRRGGBBAA по (y * sz + z) * sx + x; альфа 0 — пусто.
    std::vector<u32> cells;
    /// Размер одного вокселя в мире, в блоках.
    f32 voxelSize = 1.f / 32.f;
    /// Как смотреть на модель, рисуя значок: поворот вокруг
    /// вертикали и наклон взгляда сверху, радианы.
    f32 iconYaw   = 0.7853982f;
    f32 iconPitch = 0.5235988f;

    void resize(i32 x, i32 y, i32 z) {
        sx = x; sy = y; sz = z;
        cells.assign((usize)(x * y * z), 0u);
    }
    bool inside(i32 x, i32 y, i32 z) const {
        return (u32)x < (u32)sx && (u32)y < (u32)sy && (u32)z < (u32)sz;
    }
    u32 at(i32 x, i32 y, i32 z) const {
        return inside(x, y, z) ? cells[(usize)((y * sz + z) * sx + x)] : 0u;
    }
    bool solid(i32 x, i32 y, i32 z) const { return (at(x, y, z) & 0xFFu) != 0u; }
    void set(i32 x, i32 y, i32 z, u32 rgba) {
        if (inside(x, y, z)) cells[(usize)((y * sz + z) * sx + x)] = rgba;
    }
    usize solidCount() const;
};

/// Вершина меша модели. Цвет — байтами в порядке R, G, B, A, как их
/// читает видеокарта (см. render::packInstanceColor).
struct VoxelModelVertex {
    glm::vec3 pos;
    u32       colorGpu;
    glm::vec3 normal;
};
static_assert(sizeof(VoxelModelVertex) == 28, "формат вершины — VOXMODEL_ATTRS");

struct VoxelMesh {
    std::vector<VoxelModelVertex> vertices;
    std::vector<u32>              indices;
    /// Коробка меша в мире: низ на Y = 0, середина по X и Z.
    glm::vec3 boundsMin{0.f};
    glm::vec3 boundsMax{0.f};
    u32 quadCount() const { return (u32)(indices.size() / 6); }
};

/// Меш модели: только видимые грани, и соседние грани одного цвета
/// слиты в один прямоугольник (жадное слияние, как у чанков).
/// Обход — против часовой стрелки при взгляде снаружи, как у всего
/// проекта. Модель центрирована по X и Z и стоит на Y = 0.
VoxelMesh meshVoxelModel(const VoxelModel& m);

/// Значок: модель, отрисованная на ПРОЗРАЧНОМ фоне.
///
/// Рисуется процессором, один раз при старте: у интерфейса один атлас
/// и один вызов отрисовки на кадр, и значки ложатся в тот же атлас
/// готовыми картинками. Ортографическая проекция с позы
/// iconYaw/iconPitch, свет слева сверху, сглаживание краёв
/// двукратной выборкой и тёмный контур по силуэту — чтобы значок
/// читался на любой подложке.
///
/// `rgba` — size × size пикселей по 4 байта, строки сверху вниз, шаг
/// строки `strideBytes`. Прозрачное остаётся с альфой 0.
void renderVoxelIcon(const VoxelModel& m, u32 size, u8* rgba, u32 strideBytes);

} // namespace render
