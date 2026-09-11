#include "mesh_builder.h"
#include "../world/block.h"

namespace render {

namespace {

// Направленное освещение по индексу грани: +X, -X, +Y, -Y, +Z, -Z.
// Верх светлее низа — дешёвая замена полноценному GI.
constexpr f32 FACE_LIGHT[6] = {
    0.85f, 0.65f, 1.00f, 0.45f, 0.78f, 0.72f,
};

// Порядок обхода вершин квада для каждой грани, чтобы лицевая
// сторона была CCW при взгляде снаружи. p0=origin, p1=+du,
// p2=+du+dv, p3=+dv.
constexpr u8 WINDING[6][4] = {
    {3, 0, 1, 2},  // +X
    {2, 1, 0, 3},  // -X
    {1, 0, 3, 2},  // +Y
    {2, 3, 0, 1},  // -Y
    {0, 1, 2, 3},  // +Z
    {3, 2, 1, 0},  // -Z
};

constexpr u32 ATLAS_COLS = 16;

} // namespace

void buildChunkVertices(const world::Chunk& chunk,
                        const std::vector<world::Quad>& quads,
                        std::vector<VoxelVertex>& outVerts,
                        std::vector<u32>& outIndices)
{
    outVerts.clear();
    outIndices.clear();
    outVerts.reserve(quads.size() * 4);
    outIndices.reserve(quads.size() * 6);

    auto& reg = world::blocks();
    const glm::vec3 chunkOrigin = glm::vec3(
        (f32)(chunk.coord.x * world::CHUNK_SIZE),
        0.f,
        (f32)(chunk.coord.z * world::CHUNK_SIZE));

    for (const auto& q : quads) {
        const world::BlockDef& def = reg.get(q.v0.block);
        const u8 tileIdx = (q.v0.face == 2) ? def.atlasTop
                         : (q.v0.face == 3) ? def.atlasBottom
                                            : def.atlasSide;

        const glm::vec2 tileOrigin{
            (f32)(tileIdx % ATLAS_COLS) * ATLAS_TILE_UV,
            (f32)(tileIdx / ATLAS_COLS) * ATLAS_TILE_UV,
        };

        // Размер квада в блоках: du и dv лежат вдоль осей, поэтому
        // длина вектора и есть число тайлов по соответствующей оси.
        const f32 tilesU = glm::length(q.du);
        const f32 tilesV = glm::length(q.dv);

        const glm::vec3 corners[4] = {
            chunkOrigin + q.v0.pos,
            chunkOrigin + q.v0.pos + q.du,
            chunkOrigin + q.v0.pos + q.du + q.dv,
            chunkOrigin + q.v0.pos + q.dv,
        };
        const glm::vec2 tileUV[4] = {
            { 0.f,    0.f    },
            { tilesU, 0.f    },
            { tilesU, tilesV },
            { 0.f,    tilesV },
        };

        const u8  light = (u8)(FACE_LIGHT[q.v0.face] * 255.f);
        const u32 base  = (u32)outVerts.size();
        const u8* w     = WINDING[q.v0.face];

        for (int i = 0; i < 4; ++i) {
            const u8 idx = w[i];
            outVerts.push_back({ corners[idx], tileUV[idx], tileOrigin,
                                 light, light, light, 255 });
        }
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
    }
}

} // namespace render
