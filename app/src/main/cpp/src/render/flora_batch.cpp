/**
 * @file flora_batch.cpp
 * @brief Рендер: сборка кадра растений — экземпляры по мешам, без видеокарты.
 */
#include "flora_batch.h"
#include "../world/chunk.h"
#include "flora_models.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace render {

using world::FloraClass;
using world::FloraInstance;

void FloraBatch::begin(const glm::vec3& camera) {
    camera_ = camera;
    pending_.clear();
}

void FloraBatch::addChunk(const glm::vec3& origin, const std::vector<FloraInstance>& list) {
    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    // До ближней точки чанка: дальше дальности класса — весь класс
    // чанка мимо, и все следующие тоже (в чанке записи лежат по
    // классам, и дальность у следующего класса только короче).
    const f32 ex = std::max(std::max(origin.x - camera_.x, camera_.x - (origin.x + CH)), 0.f);
    const f32 ez = std::max(std::max(origin.z - camera_.z, camera_.z - (origin.z + CH)), 0.f);
    const f32 nearest = std::sqrt(ex * ex + ez * ez);
    const u32 models = floraModelCount();

    for (const FloraInstance& f : list) {
        const FloraClass cls = world::floraClass(f.kind);
        const FloraRange& R = FLORA_RANGES[(u32)cls];
        if (nearest > R.visible) break;
        if (pending_.size() >= MAX_INSTANCES) return;

        const bool tree = world::floraIsTree(f.kind);
        // Дерево стоит ровно на своём стволе; мелочь сдвинута внутри блока.
        const f32 px = origin.x + (f32)f.lx + 0.5f + (tree ? 0.f : world::floraOffX(f));
        const f32 pz = origin.z + (f32)f.lz + 0.5f + (tree ? 0.f : world::floraOffZ(f));
        const f32 dx = px - camera_.x, dz = pz - camera_.z;
        const f32 d = std::sqrt(dx * dx + dz * dz);
        if (d > R.visible) continue;
        const u32 lod = d < R.lod1 ? 0u : d < R.lod2 ? 1u : d < R.lod3 ? 2u : 3u;

        f32 scale = world::floraScale(f);
        // У границы дальности мелочь вырастает из земли, а не
        // выскакивает: последние восемь блоков размер идёт от нуля.
        if (cls != FloraClass::Tree) scale *= std::clamp((R.visible - d) * 0.125f, 0.f, 1.f);
        if (scale < 0.02f) continue;

        // Деревья и кактусы — с шагом в четверть оборота: они
        // крупные и стоят в сетке блоков, и повёрнутые вкось читались
        // бы чужими среди прямоугольного мира. Мелочь — как угодно.
        const u32 yaw = tree ? (u32)(f.yaw & 0xC0u) : (u32)f.yaw;
        const u32 sc = (u32)std::lround(std::clamp(scale / FLORA_MAX_SCALE, 0.f, 1.f) * 255.f);
        const u32 sway = (u32)std::lround(floraSway(f.kind) * 255.f);
        // Оттенок ±8% — по месту: два соседних куста не близнецы.
        const u32 h = (u32)f.offset * 31u + (u32)f.yaw * 17u + (u32)f.lx * 7u + (u32)f.lz * 13u;
        const u32 tone = 77u + h % 101u;

        Pending p;
        p.key = lod * models + floraModelIndex(f.kind, f.variant);
        p.inst.pos = { px, origin.y + (f32)f.y, pz };
        p.inst.params = yaw | (sc << 8) | (sway << 16) | (tone << 24);
        pending_.push_back(p);
    }
}

void FloraBatch::finish(const std::vector<u32>* meshQuads) {
    const u32 models = floraModelCount();
    const u32 keys = models * FLORA_LODS;
    counts_.assign(keys + 1, 0u);
    for (const Pending& p : pending_) ++counts_[p.key + 1];
    for (u32 k = 0; k < keys; ++k) counts_[k + 1] += counts_[k];

    // Раскладка подсчётом: ключей полторы сотни, экземпляров — тысячи.
    // Ключ идёт ступень за ступенью, поэтому ближние (первая ступень)
    // рисуются раньше дальних, и ранний тест глубины их отбрасывает.
    sorted_.resize(pending_.size());
    cursor_.assign(counts_.begin(), counts_.end() - 1);
    for (const Pending& p : pending_) sorted_[cursor_[p.key]++] = p.inst;

    draws_.clear();
    quads_ = 0;
    for (u32 key = 0; key < keys; ++key) {
        const u32 first = counts_[key], count = counts_[key + 1] - first;
        if (count == 0) continue;
        const u32 lod = key / models, model = key % models;
        const u32 mesh = model * FLORA_LODS + lod;
        const u32 q = meshQuads && mesh < meshQuads->size() ? (*meshQuads)[mesh] : 0u;
        if (meshQuads && q == 0) continue;
        draws_.push_back({ mesh, first, count });
        quads_ += count * q;
    }
}

} // namespace render
