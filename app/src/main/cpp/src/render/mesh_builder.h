#pragma once
#include "../core/types.h"
#include "../world/chunk.h"
#include <vector>
#include <glm/glm.hpp>

namespace render {

// Формат вершины. Должен соответствовать vk_pipeline vertex input.
struct VoxelVertex {
    glm::vec3 pos;      // 12
    glm::vec2 uv;       // 8
    u8 r, g, b, a;      // 4 (baked lighting / tint)
};                       // = 24

static_assert(sizeof(VoxelVertex) == 24, "VoxelVertex должен быть 24 байта");

// ============================================================
// Конвертирует greedy-меши чанка в треугольники с UV атласа
// и предвыпеченным directional-освещением.
// ============================================================
void buildChunkVertices(const world::Chunk& chunk, world::Lod lod,
                        std::vector<VoxelVertex>& outVerts,
                        std::vector<u32>& outIndices);

} // namespace render