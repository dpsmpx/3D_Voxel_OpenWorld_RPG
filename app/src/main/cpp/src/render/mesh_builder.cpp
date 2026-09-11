#include "mesh_builder.h"
#include "../world/block.h"

namespace render {

namespace {

// Направленное освещение по индексу грани
constexpr f32 FACE_LIGHT[6] = {
    0.85f, 0.65f, 1.00f, 0.45f, 0.78f, 0.72f,
};

// Порядок обхода вершин квада для каждой грани, чтобы front-face
// был CCW при взгляде снаружи. p0=origin, p1=+du, p2=+du+dv, p3=+dv.
constexpr u8 WINDING[6][4] = {
    {3, 0, 1, 2},  // +X
    {2, 1, 0, 3},  // -X
    {1, 0, 3, 2},  // +Y
    {2, 3, 0, 1},  // -Y
    {0, 1, 2, 3},  // +Z
    {3, 2, 1, 0},  // -Z
};

constexpr glm::vec2 CORNER_UV[4] = {
    {0, 0}, {1, 0}, {1, 1}, {0, 1}
};

constexpr u32 ATLAS_COLS = 16;
constexpr f32 TILE_UV    = 1.f / (f32)ATLAS_COLS;
constexpr f32 INSET      = 0.5f / 512.f;

} // namespace

void buildChunkVertices(const world::Chunk& chunk, world::Lod lod,
                        std::vector<VoxelVertex>& outVerts,
                        std::vector<u32>& outIndices)
{
    u8 lodIdx = (u8)lod;
    if (lodIdx > 3) lodIdx = 3;
    const auto& quads = chunk.meshes[lodIdx].quads;

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
        u8 tileIdx = (q.v0.face == 2) ? def.atlasTop
                   : (q.v0.face == 3) ? def.atlasBottom
                                      : def.atlasSide;

        f32 tx = (f32)(tileIdx % ATLAS_COLS) * TILE_UV;
        f32 ty = (f32)(tileIdx / ATLAS_COLS) * TILE_UV;

        f32 u0 = tx + INSET, v0 = ty + INSET;
        f32 u1 = tx + TILE_UV - INSET, v1 = ty + TILE_UV - INSET;

        glm::vec3 corners[4] = {
            chunkOrigin + q.v0.pos,
            chunkOrigin + q.v0.pos + q.du,
            chunkOrigin + q.v0.pos + q.du + q.dv,
            chunkOrigin + q.v0.pos + q.dv,
        };
        glm::vec2 tileUV[4];
        for (int i = 0; i < 4; ++i) {
            tileUV[i] = glm::vec2(
                u0 + CORNER_UV[i].x * (u1 - u0),
                v0 + CORNER_UV[i].y * (v1 - v0));
        }

        u8 light = (u8)(FACE_LIGHT[q.v0.face] * 255.f);
        u32 base = (u32)outVerts.size();

        const u8* w = WINDING[q.v0.face];
        for (int i = 0; i < 4; ++i) {
            u8 idx = w[i];
            outVerts.push_back({ corners[idx], tileUV[idx], light, light, light, 255 });
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