#pragma once
#include "../core/types.h"
#include "../world/chunk.h"
#include <vector>
#include <glm/glm.hpp>

namespace render {

// ============================================================
// Формат вершины террейна. Должен совпадать с kBindings/kAttrs
// в render_system.cpp и с входами shaders/voxel.vert.
//
// uv хранит координату в ТАЙЛАХ, а не в атласе: greedy-квад
// шириной 16 блоков получает uv до (16, 1), и шейдер повторяет
// тайл через fract(). Без этого одна текстура растягивалась на
// весь объединённый квад.
// ============================================================
struct VoxelVertex {
    glm::vec3 pos;        // 12
    glm::vec2 uv;         // 8  — координата в тайлах, может быть > 1
    glm::vec2 tileOrigin; // 8  — левый верхний угол тайла в атласе
    u8 r, g, b, a;        // 4  — запечённое освещение
};                        // = 32

static_assert(sizeof(VoxelVertex) == 32, "VoxelVertex должен быть 32 байта");

/// Размер одного тайла в UV-пространстве атласа 16x16.
constexpr f32 ATLAS_TILE_UV = 1.f / 16.f;

// ============================================================
// Конвертирует greedy-квады в треугольники с UV атласа и
// предвыпеченным направленным освещением.
// ============================================================
void buildChunkVertices(const world::Chunk& chunk,
                        const std::vector<world::Quad>& quads,
                        std::vector<VoxelVertex>& outVerts,
                        std::vector<u32>& outIndices);

} // namespace render
