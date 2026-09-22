/**
 * @file village_deeds.cpp
 * @brief Мир: следы того, что игрок сделал для деревни.
 */
#include "village_deeds.h"
#include "chunk_manager.h"
#include "block.h"
#include "../core/log.h"
#include <cmath>

namespace world {

namespace {

/// Насколько далеко от колодца житель ещё «из этой деревни».
///
/// Деревня занимает около полусотни блоков в поперечнике; сто —
/// с запасом на тех, кто отошёл к краю.
constexpr i64 VILLAGE_BELONG_R2 = 100LL * 100LL;

/// Сколько блоков вверх и вниз от поверхности искать место.
///
/// Колодец стоит на выровненной площадке, но кольцо радиусом в
/// четыре блока может задеть склон: там поверхность на пару блоков
/// выше или ниже.
constexpr i32 DEED_SEARCH_UP   = 4;
constexpr i32 DEED_SEARCH_DOWN = 4;

} // namespace

glm::ivec3 villageDeedSpot(const VillageSite& site, u32 index) {
    const f32 a = 6.2831853f * (f32)(index % VILLAGE_DEED_SPOTS)
                / (f32)VILLAGE_DEED_SPOTS;
    const i32 dx = (i32)std::lround(std::cos(a) * (f32)VILLAGE_DEED_RADIUS);
    const i32 dz = (i32)std::lround(std::sin(a) * (f32)VILLAGE_DEED_RADIUS);
    return { site.center.x + dx, site.center.y, site.center.z + dz };
}

/// Что сейчас в этом столбце кольца.
///
/// Один ответ на оба вопроса — «занято ли место» и «куда ставить».
/// Раньше это были два прохода по столбцу с разными условиями, и
/// второй ничего не решал: мутация, снявшая его целиком, не меняла
/// поведения. Два прохода вместо одного — это ещё и два места, где
/// можно разойтись в том, какой высоты столбец считается.
struct SpotState {
    bool taken = false;   ///< факел уже стоит
    i32  freeY = -1;      ///< куда встанет новый, или -1
};

static SpotState spotState(ChunkManager& world, const glm::ivec3& col) {
    VoxelReader vr(world);
    SpotState s;
    for (i32 dy = DEED_SEARCH_UP; dy >= -DEED_SEARCH_DOWN; --dy) {
        const i32 y = col.y + dy;
        if (y <= 1) continue;

        const u16 here = vr.at(col.x, y, col.z);
        if (here == TORCH) { s.taken = true; return s; }

        // Первое сверху свободное место с опорой — туда и встанет.
        // Искать от вершины мира нельзя: факел сел бы на крышу дома,
        // если она оказалась над точкой кольца.
        if (s.freeY < 0 && here == AIR && vr.isSolid(col.x, y - 1, col.z))
            s.freeY = y;
    }
    return s;
}

u32 villageDeeds(ChunkManager& world, const VillageSite& site) {
    if (!site.exists) return 0;

    u32 n = 0;
    for (u32 i = 0; i < VILLAGE_DEED_SPOTS; ++i)
        if (spotState(world, villageDeedSpot(site, i)).taken) ++n;
    return n;
}

bool lightVillageDeed(ChunkManager& world, const VillageSite& site) {
    if (!site.exists) return false;

    for (u32 i = 0; i < VILLAGE_DEED_SPOTS; ++i) {
        const glm::ivec3 col = villageDeedSpot(site, i);
        const SpotState s = spotState(world, col);
        if (s.taken || s.freeY < 0) continue;

        // setVoxel, а не запись в дельту напрямую: он же и зовёт
        // обратный вызов, которым дельта наполняется. Поставить
        // блок мимо него значило бы поставить его до первого
        // сохранения.
        world.setVoxel(col.x, s.freeY, col.z, TORCH);
        LOGI("Деревня (%d, %d): зажжён факел за сделанное",
             site.center.x, site.center.z);
        return true;
    }
    return false;
}

VillageSite villageAtPoint(ChunkManager& world, const glm::ivec3& p) {
    const i32 sx = (i32)std::floor((f32)p.x / (f32)SUPER_CHUNK_BLOCKS);
    const i32 sz = (i32)std::floor((f32)p.z / (f32)SUPER_CHUNK_BLOCKS);

    VillageSite best{};
    i64 bestD2 = VILLAGE_BELONG_R2;

    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const VillageSite v = villageAt(sx + dx, sz + dz, world.seed(),
                                            &world.generator());
            if (!v.exists) continue;
            const i64 ddx = (i64)v.center.x - p.x;
            const i64 ddz = (i64)v.center.z - p.z;
            const i64 d2 = ddx * ddx + ddz * ddz;
            if (d2 < bestD2) { bestD2 = d2; best = v; }
        }

    return best;
}

} // namespace world
