#include "chunk.h"
#include "../core/memory.h"
#include "../core/log.h"
#include <cstring>
#include <array>

namespace world {

namespace {

// Сэмплирование вокселя с учётом границ чанка — обёртка над соседями
u16 sampleVoxel(const Chunk& c, const ChunkNeighbors& nb,
                i32 x, i32 y, i32 z)
{
    if (c.inBounds(x, y, z)) return c.voxels[chunkIndex(x, y, z)];

    if (x < 0 && nb.nx) return nb.nx->at(x + CHUNK_SIZE, y, z);
    if (x >= CHUNK_SIZE && nb.px) return nb.px->at(x - CHUNK_SIZE, y, z);
    if (y < 0 && nb.ny) return nb.ny->at(x, y + CHUNK_SIZE_Y, z);
    if (y >= CHUNK_SIZE_Y && nb.py) return nb.py->at(x, y - CHUNK_SIZE_Y, z);
    if (z < 0 && nb.nz) return nb.nz->at(x, y, z + CHUNK_SIZE);
    if (z >= CHUNK_SIZE && nb.pz) return nb.pz->at(x, y, z - CHUNK_SIZE);
    return AIR;
}

// Оси граней: для каждой грани (u, v, w) и направление по w.
// face 0..5 → +X, -X, +Y, -Y, +Z, -Z
struct FaceAxes { i32 u, v, w; i32 sign; };
constexpr FaceAxes FACES[6] = {
    {1, 2, 0, +1}, // +X
    {1, 2, 0, -1}, // -X
    {0, 2, 1, +1}, // +Y
    {0, 2, 1, -1}, // -Y
    {0, 1, 2, +1}, // +Z
    {0, 1, 2, -1}, // -Z
};

// Ориентация дуги в атласе для грани — простая эвристика
inline glm::vec2 faceUv(const BlockDef& def, u8 face) {
    u8 idx = (face == 2) ? def.atlasTop
           : (face == 3) ? def.atlasBottom
                         : def.atlasSide;
    constexpr i32 ATLAS_COLS = 16;
    return { (f32)(idx % ATLAS_COLS), (f32)(idx / ATLAS_COLS) };
}

} // namespace

// ============================================================
// Greedy Meshing по 3 осям × 6 граней × слоям.
// mask[u][v] хранит block id для активной грани на текущем слое.
// ============================================================
u32 buildGreedyMesh(const Chunk& chunk, const ChunkNeighbors& nb,
                    std::vector<Quad>& outQuads, Lod lod)
{
    outQuads.clear();
    const i32 step = 1 << (u8)lod;  // шаг по осям для LOD (1, 2, 4, 8)
    const i32 sizeX = CHUNK_SIZE;
    const i32 sizeY = CHUNK_SIZE_Y;
    const i32 sizeZ = CHUNK_SIZE;

    // Размерность маски = макс(u, v)
    // u,v — поперечные оси. Размер каждой может быть до max(sizeX, sizeY, sizeZ) = sizeY.
    static thread_local std::vector<u16> maskU;
    static thread_local std::vector<u8>  maskF;
    const i32 MAX = 256;  // с запасом
    maskU.assign(MAX * MAX, 0);
    maskF.assign(MAX * MAX, 0);

    auto& reg = blocks();

    for (u8 face = 0; face < 6; ++face) {
        const FaceAxes ax = FACES[face];
        const i32 dimU = (ax.u == 0) ? sizeX : (ax.u == 1 ? sizeY : sizeZ);
        const i32 dimV = (ax.v == 0) ? sizeX : (ax.v == 1 ? sizeY : sizeZ);
        const i32 dimW = (ax.w == 0) ? sizeX : (ax.w == 1 ? sizeY : sizeZ);

        for (i32 slice = 0; slice < dimW; slice += step) {
            std::memset(maskU.data(), 0, sizeof(u16) * MAX * MAX);
            std::memset(maskF.data(), 0, sizeof(u8)  * MAX * MAX);

            for (i32 iu = 0; iu < dimU; iu += step) {
                for (i32 iv = 0; iv < dimV; iv += step) {
                    // текущий воксель
                    i32 c[3];
                    c[ax.u] = iu; c[ax.v] = iv; c[ax.w] = slice;
                    u16 cur = sampleVoxel(chunk, nb, c[0], c[1], c[2]);
                    if (cur == AIR) continue;

                    // соседний воксель
                    i32 n[3] = {c[0], c[1], c[2]};
                    n[ax.w] += ax.sign;
                    u16 neighbor = sampleVoxel(chunk, nb, n[0], n[1], n[2]);

                    const BlockDef& cd = reg.get(cur);
                    const BlockDef& nd = reg.get(neighbor);

                    bool shouldEmit = false;
                    if (cd.isSolid && !cd.isTransparent) {
                        // Прозрачный сосед → рисуем
                        if (nd.isTransparent && neighbor != cur) shouldEmit = true;
                        // Воздух → рисуем
                        if (neighbor == AIR) shouldEmit = true;
                    } else if (cd.isTransparent && !cd.isLiquid) {
                        // Листья/стекло: рисуем если сосед — воздух или другой блок
                        if (neighbor == AIR || (nd.isTransparent && !nd.isLiquid && neighbor != cur))
                            shouldEmit = true;
                    } else if (cd.isLiquid) {
                        // Вода: рисуем только верхнюю грань если над ней воздух
                        if (face == 2 && neighbor == AIR) shouldEmit = true;
                        // И боковые грани если сосед — воздух (для берегов)
                        if (face != 2 && face != 3 && neighbor == AIR) shouldEmit = true;
                    }

                    if (shouldEmit) {
                        maskU[iu * MAX + iv] = cur;
                        maskF[iu * MAX + iv] = face;
                    }
                }
            }

            // Greedy merge в плоскости (u, v)
            for (i32 iv = 0; iv < dimV; iv += step) {
                for (i32 iu = 0; iu < dimU; ) {
                    u16 b = maskU[iu * MAX + iv];
                    if (!b) { iu += step; continue; }

                    // расширяем по u
                    i32 wU = step;
                    while (iu + wU < dimU &&
                           maskU[(iu + wU) * MAX + iv] == b) wU += step;

                    // расширяем по v
                    i32 wV = step;
                    bool ok = true;
                    while (iv + wV < dimV && ok) {
                        for (i32 k = 0; k < wU; k += step) {
                            if (maskU[(iu + k) * MAX + (iv + wV)] != b) { ok = false; break; }
                        }
                        if (ok) wV += step;
                    }

                    // Формируем квад
                    Quad q{};
                    q.v0.block = b;
                    q.v0.face = face;
                    q.v0.normal = face;  // индекс нормали (0..5)

                    glm::vec3 org(0.f);
                    org[ax.u] = (f32)iu;
                    org[ax.v] = (f32)iv;
                    org[ax.w] = (f32)slice + (ax.sign > 0 ? (f32)step : 0.f);
                    q.v0.pos = org;

                    q.du = glm::vec3(0.f); q.du[ax.u] = (f32)wU;
                    q.dv = glm::vec3(0.f); q.dv[ax.v] = (f32)wV;

                    const BlockDef& def = reg.get(b);
                    q.uv = faceUv(def, face);

                    outQuads.push_back(q);

                    // стираем маску
                    for (i32 y = 0; y < wV; y += step)
                        for (i32 x = 0; x < wU; x += step)
                            maskU[(iu + x) * MAX + (iv + y)] = 0;

                    iu += wU;
                }
            }
        }
    }

    return (u32)outQuads.size();
}

} // namespace world