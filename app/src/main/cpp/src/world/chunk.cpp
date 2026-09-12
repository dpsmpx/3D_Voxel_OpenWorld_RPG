/**
 * @file chunk.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "chunk.h"
#include "../core/memory.h"
#include "../core/log.h"
#include <cstring>
#include <array>
#include <memory_resource>
#include <vector>

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

// Перекрывает ли блок свет для затенения углов. Воздух и всё
// прозрачное — нет: сквозь стекло, листву и воду угол не темнеет.
inline bool blocksLight(const BlockRegistry& reg, u16 id) {
    if (id == AIR) return false;
    const BlockDef& d = reg.get(id);
    return d.isSolid && !d.isTransparent;
}

// Затенение одного угла грани по трём соседям над ней: два «ребра»
// и «диагональ». Классическая схема воксельного AO: если оба ребра
// заняты, диагональ уже не важна — угол закрыт полностью.
// 3 — открыт, 0 — зажат.
inline u8 cornerAo(bool edge1, bool edge2, bool corner) {
    if (edge1 && edge2) return 0;
    return (u8)(3 - (u8)edge1 - (u8)edge2 - (u8)corner);
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

// Затенение всех четырёх углов грани, упакованное по два бита:
// углы p0..p3 в том же порядке, в каком мешер строит квад
// (начало, +du, +du+dv, +dv).
//
// n — клетка ПЕРЕД гранью, то есть та, из которой на грань смотрят.
// Тень даёт то, что стоит рядом с ней в плоскости грани: восемь
// соседей, из них каждый угол видит два ребра и одну диагональ.
u8 faceAo(const Chunk& c, const ChunkNeighbors& nb, const BlockRegistry& reg,
          const i32 n[3], const FaceAxes& ax, i32 step)
{
    auto occ = [&](i32 du, i32 dv) {
        i32 p[3] = { n[0], n[1], n[2] };
        p[ax.u] += du;
        p[ax.v] += dv;
        return blocksLight(reg, sampleVoxel(c, nb, p[0], p[1], p[2]));
    };
    const bool uM = occ(-step, 0),     uP = occ(step, 0);
    const bool vM = occ(0, -step),     vP = occ(0, step);
    const bool mm = occ(-step, -step), pm = occ(step, -step);
    const bool pp = occ(step, step),   mp = occ(-step, step);

    return (u8)( cornerAo(uM, vM, mm)
              | (cornerAo(uP, vM, pm) << 2)
              | (cornerAo(uP, vP, pp) << 4)
              | (cornerAo(uM, vP, mp) << 6));
}

} // namespace

// ============================================================
// Greedy Meshing по 3 осям × 6 граней × слоям.
// mask[u][v] хранит block id для активной грани на текущем слое.
// ============================================================
template<typename QuadContainer>
u32 buildGreedyMeshInto(const Chunk& chunk, const ChunkNeighbors& nb,
                        QuadContainer& outQuads, Lod lod)
{
    outQuads.clear();
    const i32 step  = 1 << (u8)lod;   // шаг по осям: 1, 2, 4, 8
    const i32 sizeX = CHUNK_SIZE;
    const i32 sizeY = CHUNK_SIZE_Y;
    const i32 sizeZ = CHUNK_SIZE;

    // Маска плоскости слоя. Раньше она была фиксированной 256x256 и
    // обнулялась целиком на каждый слой: 384 слоя x 192 КБ давали
    // ~73 МБ memset на один вызов, и это при бюджете 16 мс на чанк.
    // Теперь размер — под фактическую грань (максимум 128x128 = 32 КБ),
    // и чистится только использованная часть.
    static thread_local std::vector<u16> mask;
    // Затенение углов идёт параллельной маской: слияние требует
    // совпадения не только блока, но и всех четырёх углов.
    static thread_local std::vector<u8> aoMask;
    const i32 maxDim = sizeY > sizeX ? sizeY : sizeX;
    if ((i32)mask.size() < maxDim * maxDim) mask.assign((usize)maxDim * maxDim, 0);
    if ((i32)aoMask.size() < maxDim * maxDim) aoMask.assign((usize)maxDim * maxDim, 0);

    auto& reg = blocks();

    for (u8 face = 0; face < 6; ++face) {
        const FaceAxes ax = FACES[face];
        const i32 dimU = (ax.u == 0) ? sizeX : (ax.u == 1 ? sizeY : sizeZ);
        const i32 dimV = (ax.v == 0) ? sizeX : (ax.v == 1 ? sizeY : sizeZ);
        const i32 dimW = (ax.w == 0) ? sizeX : (ax.w == 1 ? sizeY : sizeZ);
        const i32 strideU = dimV;   // строка маски = ось V

        for (i32 slice = 0; slice < dimW; slice += step) {
            bool anyFace = false;

            for (i32 iu = 0; iu < dimU; iu += step) {
                // Чистим только ту строку, которую сейчас заполняем.
                std::memset(mask.data() + (usize)iu * strideU, 0,
                            sizeof(u16) * (usize)dimV);
                std::memset(aoMask.data() + (usize)iu * strideU, 0,
                            sizeof(u8) * (usize)dimV);

                for (i32 iv = 0; iv < dimV; iv += step) {
                    i32 c[3];
                    c[ax.u] = iu; c[ax.v] = iv; c[ax.w] = slice;
                    const u16 cur = sampleVoxel(chunk, nb, c[0], c[1], c[2]);
                    if (cur == AIR) continue;

                    i32 n[3] = { c[0], c[1], c[2] };
                    n[ax.w] += ax.sign;
                    const u16 neighbor = sampleVoxel(chunk, nb, n[0], n[1], n[2]);

                    const BlockDef& cd = reg.get(cur);
                    const BlockDef& nd = reg.get(neighbor);

                    bool shouldEmit = false;
                    if (cd.isSolid && !cd.isTransparent) {
                        // Непрозрачный блок: грань видна, если сосед
                        // пропускает свет.
                        if (neighbor == AIR) shouldEmit = true;
                        else if (nd.isTransparent && neighbor != cur) shouldEmit = true;
                    } else if (cd.isTransparent && !cd.isLiquid) {
                        // Листва и стекло: не склеиваем одинаковые блоки.
                        if (neighbor == AIR ||
                            (nd.isTransparent && !nd.isLiquid && neighbor != cur))
                            shouldEmit = true;
                    } else if (cd.isLiquid) {
                        // Вода: верхняя грань и берега.
                        if (face == 2 && neighbor == AIR) shouldEmit = true;
                        else if (face != 2 && face != 3 && neighbor == AIR) shouldEmit = true;
                    }

                    if (shouldEmit) {
                        mask[(usize)iu * strideU + iv] = cur;
                        aoMask[(usize)iu * strideU + iv] =
                            faceAo(chunk, nb, reg, n, ax, step);
                        anyFace = true;
                    }
                }
            }

            if (!anyFace) continue;   // сплошной слой внутри толщи — квадов нет

            // Жадное слияние в плоскости (u, v).
            for (i32 iv = 0; iv < dimV; iv += step) {
                for (i32 iu = 0; iu < dimU; ) {
                    const u16 b  = mask[(usize)iu * strideU + iv];
                    if (!b) { iu += step; continue; }
                    const u8  ao = aoMask[(usize)iu * strideU + iv];

                    auto same = [&](i32 u2, i32 v2) {
                        const usize k = (usize)u2 * strideU + v2;
                        return mask[k] == b && aoMask[k] == ao;
                    };

                    i32 wU = step;
                    while (iu + wU < dimU && same(iu + wU, iv)) wU += step;

                    i32 wV = step;
                    bool ok = true;
                    while (iv + wV < dimV && ok) {
                        for (i32 k = 0; k < wU; k += step) {
                            if (!same(iu + k, iv + wV)) { ok = false; break; }
                        }
                        if (ok) wV += step;
                    }

                    Quad q{};
                    q.v0.block  = b;
                    q.v0.face   = face;
                    q.v0.normal = face;

                    glm::vec3 org(0.f);
                    org[ax.u] = (f32)iu;
                    org[ax.v] = (f32)iv;
                    org[ax.w] = (f32)slice + (ax.sign > 0 ? (f32)step : 0.f);
                    q.v0.pos = org;

                    q.du = glm::vec3(0.f); q.du[ax.u] = (f32)wU;
                    q.dv = glm::vec3(0.f); q.dv[ax.v] = (f32)wV;
                    q.ao[0] = (u8)( ao       & 3);
                    q.ao[1] = (u8)((ao >> 2) & 3);
                    q.ao[2] = (u8)((ao >> 4) & 3);
                    q.ao[3] = (u8)((ao >> 6) & 3);

                    outQuads.push_back(q);

                    for (i32 y = 0; y < wV; y += step)
                        for (i32 x = 0; x < wU; x += step) {
                            const usize k = (usize)(iu + x) * strideU + (iv + y);
                            mask[k] = 0;
                            aoMask[k] = 0;
                        }

                    iu += wU;
                }
            }
        }
    }

    return (u32)outQuads.size();
}

// Явные инстанциации: обычный vector для тестов и инструментов,
// pmr-вариант для задачи меширования поверх арены воркера.
u32 buildGreedyMesh(const Chunk& chunk, const ChunkNeighbors& nb,
                    std::vector<Quad>& outQuads, Lod lod) {
    return buildGreedyMeshInto(chunk, nb, outQuads, lod);
}

u32 buildGreedyMesh(const Chunk& chunk, const ChunkNeighbors& nb,
                    std::pmr::vector<Quad>& outQuads, Lod lod) {
    return buildGreedyMeshInto(chunk, nb, outQuads, lod);
}

} // namespace world
