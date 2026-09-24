/**
 * @file mesh_builder.cpp
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#include "mesh_builder.h"
#include "../world/block.h"

#include <vector>

namespace render {

namespace {

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

/// Полупрозрачна ли грань. Смотрим не на флаг блока, а на альфу
/// самого цвета: листва и лава помечены прозрачными, чтобы мешер не
/// склеивал их грани, но рисуются они непрозрачными — гонять их через
/// проход со смешиванием незачем.
inline bool faceIsBlended(const world::BlockDef& def, u8 face) {
    return (def.faceColor(face) & 0xFFu) < 255u;
}

/// Выкладывает один квад: четыре вершины и шесть индексов.
void emitQuad(const world::Quad& q, const world::BlockDef& def,
              std::vector<VoxelVertex>& verts, std::vector<u32>& idx,
              bool forceOpaque = false)
{
    const u32 rgba = def.faceColor(q.v0.face);
    const u8 cr = (u8)(rgba >> 24), cg = (u8)(rgba >> 16);
    const u8 cb = (u8)(rgba >>  8);
    const bool waterTop = q.v0.block == world::WATER && q.v0.face == 2;
    // Top-water alpha is reused as an 8-bit flow payload. The fragment
    // shader restores the real material alpha, so VoxelVertex stays 8 bytes.
    const u8 ca = forceOpaque ? (u8)255
                              : (waterTop ? q.waterFlow : (u8)(rgba));

    const glm::vec3 corners[4] = {
        q.v0.pos,
        q.v0.pos + q.du,
        q.v0.pos + q.du + q.dv,
        q.v0.pos + q.dv,
    };

    const u32 base = (u32)verts.size();
    const u8* w    = WINDING[q.v0.face];

    u8 ao[4];
    for (int i = 0; i < 4; ++i) {
        const u8 c = w[i];
        const glm::vec3& p = corners[c];
        ao[i] = q.ao[c];
        verts.push_back({
            packVoxelPos((u32)p.x, (u32)p.y, (u32)p.z,
                         q.v0.face, ao[i], q.sky[c], def.colorJitter),
            cr, cg, cb, ca });
    }

    // Разрез квада по диагонали. Если затенение углов несимметрично,
    // разрез обязан идти через два ТЁМНЫХ угла: иначе интерполяция
    // размажет тень поперёк квада, и на ровной стене проступит
    // характерная «складка» из двух треугольников.
    if (ao[0] + ao[2] > ao[1] + ao[3]) {
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    } else {
        idx.push_back(base + 1); idx.push_back(base + 2); idx.push_back(base + 3);
        idx.push_back(base + 1); idx.push_back(base + 3); idx.push_back(base + 0);
    }
}

} // namespace

void buildChunkVertices(const world::Chunk& chunk,
                        const std::vector<world::Quad>& quads,
                        std::vector<VoxelVertex>& outVerts,
                        std::vector<u32>& outIndices,
                        u32& outOpaqueIndices,
                        glm::vec3* outBlendCenter)
{
    (void)chunk;   // позиции локальные: смещение чанка добавляет шейдер
    // Прозрачное смешивается всегда: уровней детализации, на дальних
    // из которых вода рисовалась непрозрачной, больше нет.
    constexpr bool blendAllowed = true;
    if (outBlendCenter) *outBlendCenter = glm::vec3(0.f);
    outVerts.clear();
    outIndices.clear();
    outVerts.reserve(quads.size() * 4);
    outIndices.reserve(quads.size() * 6);

    auto& reg = world::blocks();

    // Два прохода по одному списку: сначала непрозрачное, потом всё
    // остальное. Порядок в буфере и есть порядок отрисовки.
    for (const auto& q : quads) {
        const world::BlockDef& def = reg.get(q.v0.block);
        const bool blended = faceIsBlended(def, q.v0.face);
        if (blended && blendAllowed) continue;
        emitQuad(q, def, outVerts, outIndices, blended);
    }
    outOpaqueIndices = (u32)outIndices.size();
    if (!blendAllowed) return;   // полупрозрачного хвоста на огрублённых нет

    glm::dvec3 blendSum(0.0);
    f64 blendArea = 0.0;
    for (const auto& q : quads) {
        const world::BlockDef& def = reg.get(q.v0.block);
        if (!faceIsBlended(def, q.v0.face)) continue;
        emitQuad(q, def, outVerts, outIndices);
        // Взвешиваем площадью: слитый greedy-квад в сотню блоков
        // должен значить больше одинокого квада на краю пруда.
        const glm::vec3 c = q.v0.pos + (q.du + q.dv) * 0.5f;
        const f64 area = (f64)glm::length(glm::cross(q.du, q.dv));
        const f64 w = area > 0.0 ? area : 1.0;
        blendSum += glm::dvec3(c) * w;
        blendArea += w;
    }
    if (outBlendCenter && blendArea > 0.0)
        *outBlendCenter = glm::vec3(blendSum / blendArea);
}

} // namespace render
