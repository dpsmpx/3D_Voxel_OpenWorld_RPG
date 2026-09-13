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

// Сэмплирование вокселя с учётом границ чанка — обёртка над соседями.
// Объявлена в chunk.h: договор о трёх исходах проверяется тестом.
u16 sampleVoxel(const Chunk& c, const ChunkNeighbors& nb,
                i32 x, i32 y, i32 z)
{
    if (c.inBounds(x, y, z)) return c.voxels[chunkIndex(x, y, z)];

    if (x < 0)             return nb.nx ? nb.nx->at(x + CHUNK_SIZE, y, z) : UNKNOWN;
    if (x >= CHUNK_SIZE)   return nb.px ? nb.px->at(x - CHUNK_SIZE, y, z) : UNKNOWN;
    if (z < 0)             return nb.nz ? nb.nz->at(x, y, z + CHUNK_SIZE) : UNKNOWN;
    if (z >= CHUNK_SIZE)   return nb.pz ? nb.pz->at(x, y, z - CHUNK_SIZE) : UNKNOWN;

    // Выше потолка мира — настоящий воздух: там ничего нет и не будет.
    if (y >= CHUNK_SIZE_Y) return nb.py ? nb.py->at(x, y - CHUNK_SIZE_Y, z) : AIR;
    // Ниже дна — не воздух: наружу дно мира смотреть не должно, иначе
    // мешер строит по нему грань, которую никто никогда не увидит.
    if (y < 0)             return nb.ny ? nb.ny->at(x, y + CHUNK_SIZE_Y, z) : UNKNOWN;
    return AIR;
}

namespace {

// Перекрывает ли блок свет для затенения углов. Воздух и всё
// прозрачное — нет: сквозь стекло, листву и воду угол не темнеет.
inline bool blocksLight(const BlockRegistry& reg, u16 id) {
    if (id == AIR) return false;
    // Незагруженный сосед свет не перекрывает: иначе на стыке
    // появлялась бы тень от блока, которого, может, и нет вовсе.
    // Когда сосед подгрузится, чанк перемешируется и посчитает честно.
    if (id == UNKNOWN) return false;
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

// ============================================================
// Объём для меширования.
//
// Уровень детализации — это НЕ «брать каждый N-й воксель». Так было
// раньше, и так получался мусор: грань решалась по соседу в одном
// вокселе, а покрывала N, поверхность между точками выборки исчезала
// целиком, и в воздухе оставались висеть отдельные плиты.
//
// Правильно — огрубить сам объём: куб N x N x N вокселей сводится к
// одной клетке, и дальше всё работает как обычно, с шагом в одну
// клетку. Затенение углов и открытость неба тоже считаются по
// огрублённым клеткам, поэтому они остаются осмысленными, а слияние
// граней не дробится на полоски.
// ============================================================
struct MeshVolume {
    const Chunk*          chunk = nullptr;
    const ChunkNeighbors* nb    = nullptr;
    i32 step  = 1;          ///< сколько вокселей в клетке по ребру
    i32 dimX = CHUNK_SIZE, dimY = CHUNK_SIZE_Y, dimZ = CHUNK_SIZE;

    /// Огрублённый объём с рамкой в одну клетку. Пуст при step == 1.
    const std::vector<u16>* coarse = nullptr;
    i32 cw = 0, ch = 0, cd = 0;

    u16 at(i32 x, i32 y, i32 z) const {
        if (step == 1) return sampleVoxel(*chunk, *nb, x, y, z);
        const i32 ix = x + 1, iy = y + 1, iz = z + 1;
        if ((u32)ix >= (u32)cw || (u32)iy >= (u32)ch || (u32)iz >= (u32)cd) return AIR;
        return (*coarse)[((usize)iy * (usize)cd + (usize)iz) * (usize)cw + (usize)ix];
    }
};

/// Сводит куб step^3 вокселей к одной клетке.
///
/// Клетка непуста, если в ней есть хоть один непустой воксель, а
/// материал берётся от САМОГО ВЕРХНЕГО из них — того, который видно
/// снаружи. Порог «хотя бы четверть куба» казался разумным, но
/// поверхность мира — это один слой травы поверх толщи: при шаге
/// восемь она даёт восьмую часть клетки и пропадала целиком, а там,
/// где рельеф чуть ниже порога, в земле открывались дыры насквозь.
/// Выбирать самый частый блок нельзя по той же причине: в клетке, где
/// трава лежит на камне, камня больше — и дальний луг становился серым.
///
/// Отдельное правило для ЖИДКОСТЕЙ, и вот почему.
///
/// Вода прозрачна, и клетка-вода — это дыра в непрозрачном рельефе.
/// Правило «верхний воксель выигрывает» отдавало такой клетке весь
/// куб, стоило воде оказаться выше кромки берега хоть в одной
/// колонке. При шаге 4 берег ещё делился на четыре клетки и вода
/// оставалась в своей, а при шаге 8 те же блоки сходились в одну —
/// и полоса суши шириной в полклетки становилась прозрачной: сквозь
/// дальний берег было видно небо. Обратный случай так же груб: если
/// верхней оказывалась суша, озеро внутри клетки исчезало целиком.
/// То есть вода то разливалась, то пропадала ровно на переходе
/// 4³ → 8³, и это читалось как мигание на горизонте.
///
/// Теперь решают КОЛОНКИ, а не один воксель: у каждой колонки клетки
/// берётся её верхний непустой блок, и клетка становится водой лишь
/// тогда, когда вода наверху у БОЛЬШИНСТВА колонок. Правило не
/// зависит от порядка обхода, не отдаёт клетку одному вокселю и
/// меняется плавно: озеро шире половины клетки останется озером на
/// любом шаге, а берег останется сушей.
u16 collapseCell(const BlockRegistry& reg, const Chunk& c,
                 const ChunkNeighbors& nb,
                 i32 cx, i32 cy, i32 cz, i32 step)
{
    const i32 bx = cx * step, by = cy * step, bz = cz * step;

    i32 solidCols = 0,  liquidCols = 0;
    u16 topSolid  = AIR; i32 topSolidY  = -1;
    u16 topLiquid = AIR; i32 topLiquidY = -1;
    bool anyUnknown = false;

    for (i32 z = 0; z < step; ++z) {
        for (i32 x = 0; x < step; ++x) {
            for (i32 y = step - 1; y >= 0; --y) {
                const u16 v = sampleVoxel(c, nb, bx + x, by + y, bz + z);
                // Неизвестное не блок и не воздух: запоминаем и идём
                // дальше. Если во всей клетке не нашлось ни одного
                // настоящего блока, клетка остаётся неизвестной —
                // мешер по ней грань не построит.
                if (v == UNKNOWN) { anyUnknown = true; continue; }
                if (v == AIR) continue;
                if (reg.get(v).isLiquid) {
                    ++liquidCols;
                    if (y > topLiquidY) { topLiquidY = y; topLiquid = v; }
                } else {
                    ++solidCols;
                    if (y > topSolidY) { topSolidY = y; topSolid = v; }
                }
                break;   // ниже верхнего блока колонки смотреть незачем
            }
        }
    }

    if (solidCols == 0 && liquidCols == 0)
        return anyUnknown ? UNKNOWN : AIR;
    // Ничья достаётся суше: лишний воксель берега на горизонте не
    // виден, а дыра в берегу видна сразу.
    if (liquidCols > solidCols) return topLiquid;
    return topSolid != AIR ? topSolid : topLiquid;
}

void buildCoarse(const BlockRegistry& reg, const Chunk& c,
                 const ChunkNeighbors& nb, i32 step,
                 std::vector<u16>& out, i32& cw, i32& ch, i32& cd)
{
    const i32 dx = CHUNK_SIZE / step, dy = CHUNK_SIZE_Y / step, dz = CHUNK_SIZE / step;
    cw = dx + 2; ch = dy + 2; cd = dz + 2;
    out.assign((usize)cw * ch * cd, (u16)AIR);
    for (i32 cy = -1; cy <= dy; ++cy)
        for (i32 cz = -1; cz <= dz; ++cz)
            for (i32 cx = -1; cx <= dx; ++cx)
                out[((usize)(cy + 1) * cd + (cz + 1)) * cw + (cx + 1)] =
                    collapseCell(reg, c, nb, cx, cy, cz, step);
}

// Высота верхней непрозрачной клетки в колонке, -1 если колонка
// пуста. Считается один раз на чанк, с рамкой в одну клетку, чтобы
// грани на стыке смотрели в соседний чанк, а не в пустоту — иначе на
// границах пошёл бы шов из светлых пикселей.
//
// Такой «столбик сверху» — не полноценное распространение света, зато
// у него нет ни очередей, ни пересчёта соседей, ни швов: он смотрит
// строго вверх, а вертикаль чанка не покидает. Пещеры и навесы он
// отделяет от открытого склона, а это главное.
constexpr i32 SKY_BORDER = 1;

struct SkyMap {
    std::vector<i16> top;
    i32 dim = 0;        ///< ширина с рамкой
    i32 dimZ = 0;

    i16 at(i32 x, i32 z) const {
        const i32 ix = x + SKY_BORDER, iz = z + SKY_BORDER;
        if ((u32)ix >= (u32)dim || (u32)iz >= (u32)dimZ) return -1;
        return top[(usize)iz * dim + ix];
    }
};

void buildTopSolid(const MeshVolume& vol, const BlockRegistry& reg, SkyMap& sky) {
    sky.dim  = vol.dimX + SKY_BORDER * 2;
    sky.dimZ = vol.dimZ + SKY_BORDER * 2;
    sky.top.assign((usize)sky.dim * sky.dimZ, (i16)-1);
    for (i32 z = -SKY_BORDER; z < vol.dimZ + SKY_BORDER; ++z) {
        for (i32 x = -SKY_BORDER; x < vol.dimX + SKY_BORDER; ++x) {
            i32 y = vol.dimY - 1;
            for (; y >= 0; --y)
                if (blocksLight(reg, vol.at(x, y, z))) break;
            sky.top[(usize)(z + SKY_BORDER) * sky.dim + (x + SKY_BORDER)] = (i16)y;
        }
    }
}

/// Открытость неба в клетке, 0..7. Над верхней непрозрачной клеткой —
/// полностью открыто; ниже гаснет за пять клеток. Остаток не нулевой:
/// в абсолютной темноте пещера не читается, она просто исчезает.
inline u8 skyAt(const SkyMap& sky, i32 x, i32 y, i32 z) {
    const i32 t = sky.at(x, z);
    if (y > t) return 7;
    const i32 depth = t - y + 1;
    const i32 v = 7 - (depth * 7 + 2) / 5;
    return (u8)(v < 0 ? 0 : v);
}

// Открытость неба во всех четырёх углах грани, по три бита на угол.
// Угол видит четыре клетки, сходящиеся в нём: усреднение по ним даёт
// плавный переход вместо ступеньки в одну клетку на входе в пещеру.
u16 faceSky(const SkyMap& sky, const i32 n[3], const FaceAxes& ax) {
    auto at = [&](i32 du, i32 dv) {
        i32 p[3] = { n[0], n[1], n[2] };
        p[ax.u] += du;
        p[ax.v] += dv;
        return (i32)skyAt(sky, p[0], p[1], p[2]);
    };
    const i32 c  = at(0, 0);
    const i32 uM = at(-1, 0),  uP = at(1, 0);
    const i32 vM = at(0, -1),  vP = at(0, 1);
    const i32 mm = at(-1, -1), pm = at(1, -1);
    const i32 pp = at(1, 1),   mp = at(-1, 1);

    auto avg = [](i32 a, i32 b, i32 d, i32 e) {
        return (u16)((a + b + d + e + 2) / 4);
    };
    return (u16)( avg(c, uM, vM, mm)
              | (avg(c, uP, vM, pm) << 3)
              | (avg(c, uP, vP, pp) << 6)
              | (avg(c, uM, vP, mp) << 9));
}

// Затенение всех четырёх углов грани, упакованное по два бита:
// углы p0..p3 в том же порядке, в каком мешер строит квад
// (начало, +du, +du+dv, +dv).
//
// n — клетка ПЕРЕД гранью, то есть та, из которой на грань смотрят.
// Тень даёт то, что стоит рядом с ней в плоскости грани: восемь
// соседей, из них каждый угол видит два ребра и одну диагональ.
u8 faceAo(const MeshVolume& vol, const BlockRegistry& reg,
          const i32 n[3], const FaceAxes& ax)
{
    auto occ = [&](i32 du, i32 dv) {
        i32 p[3] = { n[0], n[1], n[2] };
        p[ax.u] += du;
        p[ax.v] += dv;
        return blocksLight(reg, vol.at(p[0], p[1], p[2]));
    };
    const bool uM = occ(-1, 0),  uP = occ(1, 0);
    const bool vM = occ(0, -1),  vP = occ(0, 1);
    const bool mm = occ(-1, -1), pm = occ(1, -1);
    const bool pp = occ(1, 1),   mp = occ(-1, 1);

    return (u8)( cornerAo(uM, vM, mm)
              | (cornerAo(uP, vM, pm) << 2)
              | (cornerAo(uP, vP, pp) << 4)
              | (cornerAo(uM, vP, mp) << 6));
}

} // namespace

// ============================================================
// Greedy Meshing по 3 осям x 6 граней x слоям.
// Всё внутри — в координатах КЛЕТОК выбранного уровня детализации;
// в блоки они переводятся один раз, при записи квада.
// ============================================================
template<typename QuadContainer>
u32 buildGreedyMeshInto(const Chunk& chunk, const ChunkNeighbors& nb,
                        QuadContainer& outQuads, Lod lod)
{
    outQuads.clear();
    const i32 step = 1 << (u8)lod;   // вокселей в клетке: 1, 2, 4, 8

    static thread_local std::vector<u16> coarse;
    MeshVolume vol;
    vol.chunk = &chunk;
    vol.nb    = &nb;
    vol.step  = step;
    vol.dimX  = CHUNK_SIZE   / step;
    vol.dimY  = CHUNK_SIZE_Y / step;
    vol.dimZ  = CHUNK_SIZE   / step;
    if (step > 1) {
        buildCoarse(blocks(), chunk, nb, step, coarse, vol.cw, vol.ch, vol.cd);
        vol.coarse = &coarse;
    }

    // Маска плоскости слоя. Раньше она была фиксированной 256x256 и
    // обнулялась целиком на каждый слой: 384 слоя x 192 КБ давали
    // ~73 МБ memset на один вызов, и это при бюджете 16 мс на чанк.
    // Теперь размер — под фактическую грань, и чистится только
    // использованная часть.
    static thread_local std::vector<u16> mask;
    // Затенение углов и открытость неба идут параллельными масками:
    // слияние требует совпадения всех трёх, иначе тень свода
    // растеклась бы по освещённому склону.
    static thread_local std::vector<u8>  aoMask;
    static thread_local std::vector<u16> skyMask;

    const i32 maxDim = vol.dimY > vol.dimX ? vol.dimY : vol.dimX;
    const usize need = (usize)maxDim * maxDim;
    if (mask.size()    < need) mask.assign(need, 0);
    if (aoMask.size()  < need) aoMask.assign(need, 0);
    if (skyMask.size() < need) skyMask.assign(need, 0);

    static thread_local SkyMap skyMap;
    auto& reg = blocks();
    buildTopSolid(vol, reg, skyMap);

    for (u8 face = 0; face < 6; ++face) {
        const FaceAxes ax = FACES[face];
        const i32 dimU = (ax.u == 0) ? vol.dimX : (ax.u == 1 ? vol.dimY : vol.dimZ);
        const i32 dimV = (ax.v == 0) ? vol.dimX : (ax.v == 1 ? vol.dimY : vol.dimZ);
        const i32 dimW = (ax.w == 0) ? vol.dimX : (ax.w == 1 ? vol.dimY : vol.dimZ);
        const i32 strideU = dimV;   // строка маски = ось V

        for (i32 slice = 0; slice < dimW; ++slice) {
            bool anyFace = false;

            for (i32 iu = 0; iu < dimU; ++iu) {
                // Чистим только ту строку, которую сейчас заполняем.
                std::memset(mask.data()    + (usize)iu * strideU, 0, sizeof(u16) * (usize)dimV);
                std::memset(aoMask.data()  + (usize)iu * strideU, 0, sizeof(u8)  * (usize)dimV);
                std::memset(skyMask.data() + (usize)iu * strideU, 0, sizeof(u16) * (usize)dimV);

                for (i32 iv = 0; iv < dimV; ++iv) {
                    i32 c[3];
                    c[ax.u] = iu; c[ax.v] = iv; c[ax.w] = slice;
                    const u16 cur = vol.at(c[0], c[1], c[2]);
                    if (cur == AIR || cur == UNKNOWN) continue;

                    i32 n[3] = { c[0], c[1], c[2] };
                    n[ax.w] += ax.sign;
                    const u16 neighbor = vol.at(n[0], n[1], n[2]);

                    // Сосед неизвестен — решать нельзя.
                    //
                    // Три состояния, а не два: сосед есть и там воздух
                    // (грань открыта), сосед есть и там блок (грань
                    // закрыта), соседнего чанка ещё нет (неизвестно).
                    // Раньше третье считалось первым, и по краю
                    // незагруженного чанка вырастала наружная стена во
                    // всю толщу земли. Когда сосед подгрузится,
                    // ChunkManager перестроит этот чанк, и настоящая
                    // граница появится сама.
                    if (neighbor == UNKNOWN) continue;

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
                        const usize k = (usize)iu * strideU + iv;
                        mask[k]    = cur;
                        aoMask[k]  = faceAo(vol, reg, n, ax);
                        skyMask[k] = faceSky(skyMap, n, ax);
                        anyFace = true;
                    }
                }
            }

            if (!anyFace) continue;   // сплошной слой внутри толщи — квадов нет

            // Жадное слияние в плоскости (u, v).
            for (i32 iv = 0; iv < dimV; ++iv) {
                for (i32 iu = 0; iu < dimU; ) {
                    const usize k0 = (usize)iu * strideU + iv;
                    const u16 b = mask[k0];
                    if (!b) { ++iu; continue; }
                    const u8  ao  = aoMask[k0];
                    const u16 sky = skyMask[k0];

                    auto same = [&](i32 u2, i32 v2) {
                        const usize k = (usize)u2 * strideU + v2;
                        return mask[k] == b && aoMask[k] == ao && skyMask[k] == sky;
                    };

                    i32 wU = 1;
                    while (iu + wU < dimU && same(iu + wU, iv)) ++wU;

                    i32 wV = 1;
                    bool ok = true;
                    while (iv + wV < dimV && ok) {
                        for (i32 k = 0; k < wU; ++k)
                            if (!same(iu + k, iv + wV)) { ok = false; break; }
                        if (ok) ++wV;
                    }

                    Quad q{};
                    q.v0.block  = b;
                    q.v0.face   = face;
                    q.v0.normal = face;

                    // Из клеток в блоки — ровно здесь и один раз.
                    glm::vec3 org(0.f);
                    org[ax.u] = (f32)(iu * step);
                    org[ax.v] = (f32)(iv * step);
                    org[ax.w] = (f32)((slice + (ax.sign > 0 ? 1 : 0)) * step);
                    q.v0.pos = org;

                    q.du = glm::vec3(0.f); q.du[ax.u] = (f32)(wU * step);
                    q.dv = glm::vec3(0.f); q.dv[ax.v] = (f32)(wV * step);

                    q.ao[0] = (u8)( ao       & 3);
                    q.ao[1] = (u8)((ao >> 2) & 3);
                    q.ao[2] = (u8)((ao >> 4) & 3);
                    q.ao[3] = (u8)((ao >> 6) & 3);
                    q.sky[0] = (u8)( sky        & 7);
                    q.sky[1] = (u8)((sky >>  3) & 7);
                    q.sky[2] = (u8)((sky >>  6) & 7);
                    q.sky[3] = (u8)((sky >>  9) & 7);

                    outQuads.push_back(q);

                    for (i32 y = 0; y < wV; ++y)
                        for (i32 x = 0; x < wU; ++x) {
                            const usize k = (usize)(iu + x) * strideU + (iv + y);
                            mask[k] = 0; aoMask[k] = 0; skyMask[k] = 0;
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
