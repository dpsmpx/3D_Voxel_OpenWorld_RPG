/**
 * @file lights.cpp
 * @brief Точечные источники света: факелы, фонари, лава, огонь в руке.
 */
#include "lights.h"
#include "../world/block.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace render {

void LightField::scan(world::ChunkManager& world, const glm::vec3& around,
                      f32 dt)
{
    timer_ += dt;
    const bool moved = glm::length(around - lastScan_) > RESCAN_MOVE;
    if (scanned_ && !moved && timer_ < RESCAN_SEC) return;
    timer_ = 0.f;
    lastScan_ = around;
    scanned_ = true;

    blocks_.clear();

    const i32 cx = (i32)std::floor(around.x);
    const i32 cy = (i32)std::floor(around.y);
    const i32 cz = (i32)std::floor(around.z);

    // Курсор, а не getVoxel на каждый блок: перебор идёт по соседним
    // клеткам, то есть почти всегда по одному и тому же чанку, и
    // поиск чанка с двумя захватами замков здесь — основная цена.
    world::VoxelReader rd(world);

    const i32 y0 = std::max(0, cy - SCAN_RADIUS_Y);
    const i32 y1 = std::min(world::CHUNK_SIZE_Y - 1, cy + SCAN_RADIUS_Y);

    for (i32 y = y0; y <= y1; ++y)
        for (i32 z = cz - SCAN_RADIUS_XZ; z <= cz + SCAN_RADIUS_XZ; ++z)
            for (i32 x = cx - SCAN_RADIUS_XZ; x <= cx + SCAN_RADIUS_XZ; ++x) {
                const u16 b = rd.at(x, y, z);
                if (b == world::AIR || b == world::UNKNOWN) continue;
                const PointLight l = lightOfBlock(b, { x, y, z });
                if (l.power > 0.f) blocks_.push_back(l);
            }
}

void LightField::addTransient(const PointLight& l) {
    if (l.power <= 0.f) return;
    transient_.push_back(l);
}

u32 LightField::nearest(const glm::vec3& to, PointLight* out, u32 maxOut) const {
    if (!out || maxOut == 0) return 0;

    // Подвижные огни идут первыми и всегда: факел в руке не должен
    // выбиваться из списка стеной факелов в двух шагах позади.
    u32 n = 0;
    for (const PointLight& l : transient_) {
        if (n >= maxOut) return n;
        out[n++] = l;
    }

    // Блоки — по расстоянию до точки, ближние вперёд. Частичной
    // сортировкой: полная сортировка сотни светильников ради восьми
    // ближних — работа впустую.
    struct Scored { f32 d2; const PointLight* l; };
    std::vector<Scored> scored;
    scored.reserve(blocks_.size());
    for (const PointLight& l : blocks_) {
        const glm::vec3 d = l.pos - to;
        scored.push_back({ glm::dot(d, d), &l });
    }
    const usize take = std::min<usize>(scored.size(), maxOut - n);
    std::partial_sort(scored.begin(), scored.begin() + (std::ptrdiff_t)take, scored.end(),
                      [](const Scored& a, const Scored& b) { return a.d2 < b.d2; });
    for (usize i = 0; i < take; ++i) out[n++] = *scored[i].l;
    return n;
}

} // namespace render
