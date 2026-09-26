// ============================================================
// tools/rivercheck — гидрологическая проверка генератора рек.
//
// Строит плитки гидросети для нескольких миров и проверяет свойства
// всей сети, а не одной реки:
//   * каждое русло, начатое в центральной плитке, доходит до моря
//     (через озёра, слияния и швы плиток) без тупиков и петель;
//   * вода не поднимается вниз по течению ни на ребре, ни на стыке;
//   * сток вниз по течению не убывает, после слияния — не меньше
//     суммы притоков;
//   * ребро, ушедшее в соседнюю плитку, продолжается там из той же
//     точки на той же высоте;
//   * у сети есть притоки нескольких порядков, озёра и пороги.
// И замеряет, сколько стоит плитка и сколько — чанк с рекой.
//
// Запускается из tools/hostcheck/run.sh.
// ============================================================
#include "world/features.h"
#include "world/terrain.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace {

namespace hy = world::hydro;

i32 tileOfNode(i32 n) { return hy::tileOfNode(n); }

struct Stats {
    i32 paths = 0, chains = 0, reachedSea = 0, leftArea = 0;
    i32 deadEnds = 0, loops = 0, rises = 0, flowDrops = 0, seamBreaks = 0;
    i32 lakes = 0, deltas = 0, falls = 0, braided = 0;
    u8  maxOrder = 0;
};

bool checkWorld(u64 seed, Stats& st) {
    world::TerrainGenerator gen(seed);
    const auto& hydro = gen.hydrology();

    const auto t0 = std::chrono::steady_clock::now();
    std::map<std::pair<i32, i32>, std::shared_ptr<const hy::Tile>> tiles;
    for (i32 tz = -1; tz <= 1; ++tz)
        for (i32 tx = -1; tx <= 1; ++tx)
            tiles[{ tx, tz }] = hydro.tile(tx, tz);
    const f64 ms = std::chrono::duration<f64, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    // Путь по узлу-истоку: для прохода цепочки вниз по течению.
    std::map<std::pair<i32, i32>, std::pair<const hy::Tile*, const hy::Path*>> from;
    for (const auto& [key, T] : tiles)
        for (const auto& p : T->paths) {
            if (p.kind == hy::PATH_DELTA) { ++st.deltas; continue; }
            from[{ p.fromI, p.fromJ }] = { T.get(), &p };
            ++st.paths;
            if (p.order > st.maxOrder) st.maxOrder = p.order;
            for (u32 s = 1; s < p.count; ++s) {
                const auto& a = T->samples[p.first + s - 1];
                const auto& b = T->samples[p.first + s];
                if (b.waterY > a.waterY) ++st.rises;
                if (b.flags & hy::SF_PLUNGE) ++st.falls;
                if (b.braid > 0.5f) ++st.braided;
            }
        }
    for (const auto& [key, T] : tiles)
        for (u8 f : T->nodeFlags) if (f & hy::NF_LAKE) ++st.lakes;

    // Приток сливается: сумма стоков, приходящих в узел.
    std::map<std::pair<i32, i32>, f32> inflow;
    for (const auto& [node, tp] : from)
        if (!tp.second->mouth) inflow[{ tp.second->toI, tp.second->toJ }] += tp.second->flow;
    for (const auto& [node, sum] : inflow) {
        auto it = from.find(node);
        if (it == from.end()) continue;
        if (it->second.second->flow + 1e-3f < sum) {
            ++st.flowDrops;
            if (st.flowDrops < 4)
                std::fprintf(stderr, "rivercheck: сток падает после слияния в узле %d,%d: %.1f < %.1f\n",
                             node.first, node.second, it->second.second->flow, sum);
        }
    }

    // Цепочки из центральной плитки — до моря.
    const auto& center = tiles[{ 0, 0 }];
    for (const auto& p0 : center->paths) {
        if (p0.kind != hy::PATH_RIVER) continue;
        ++st.chains;
        const hy::Tile* T = center.get();
        const hy::Path* p = &p0;
        std::set<std::pair<i32, i32>> seen;
        bool done = false;
        for (i32 step = 0; step < 4000 && !done; ++step) {
            if (!seen.insert({ p->fromI, p->fromJ }).second) { ++st.loops; done = true; break; }
            if (p->mouth) { ++st.reachedSea; done = true; break; }
            if (!tiles.count({ tileOfNode(p->toI), tileOfNode(p->toJ) })) {
                ++st.leftArea; done = true; break;
            }
            auto it = from.find({ p->toI, p->toJ });
            if (it == from.end()) {
                ++st.deadEnds;
                if (st.deadEnds < 4)
                    std::fprintf(stderr, "rivercheck: русло обрывается в узле %d,%d (seed %llx)\n",
                                 p->toI, p->toJ, (unsigned long long)seed);
                done = true;
                break;
            }
            const hy::Tile* U = it->second.first;
            const hy::Path* q = it->second.second;
            const auto& end = T->samples[p->first + p->count - 1];
            const auto& start = U->samples[q->first];
            if (std::fabs(end.x - start.x) > 0.01f || std::fabs(end.z - start.z) > 0.01f ||
                end.waterY != start.waterY)
                ++st.seamBreaks;
            if (start.waterY > end.waterY) ++st.rises;
            if (q->flow + 1e-3f < p->flow) ++st.flowDrops;
            T = U;
            p = q;
        }
        if (!done) ++st.loops;
    }

    std::printf("rivercheck: seed %llx: 9 плиток за %.0f мс, рёбер %d\n",
                (unsigned long long)seed, ms, st.paths);
    return true;
}

/// Цена чанка с рекой: плитки уже в кэше, как во время игры.
void timeChunks(u64 seed) {
    world::TerrainGenerator gen(seed);
    // Плитки — заранее: во время игры их готовит фон, и в цену чанка
    // они не входят.
    for (i32 tz = -1; tz <= 1; ++tz)
        for (i32 tx = -1; tx <= 1; ++tx) (void)gen.hydrology().tile(tx, tz);
    const auto T = gen.hydrology().tile(0, 0);
    std::set<std::pair<i32, i32>> wanted;
    for (const auto& p : T->paths) {
        const auto& mid = T->samples[p.first + p.count / 2];
        wanted.insert({ (i32)std::floor(mid.x / 32.f), (i32)std::floor(mid.z / 32.f) });
        if (wanted.size() >= 64) break;
    }
    auto chunk = std::make_unique<world::Chunk>();
    std::vector<world::TerrainGenerator::Column> cols;
    world::hydro::ChunkView view;
    f64 riverMs = 0.0;
    for (const auto& [cx, cz] : wanted) {
        chunk->coord = { cx, 0, cz };
        world::computeChunkColumns(gen, cx, cz, cols);
        const auto a = std::chrono::steady_clock::now();
        std::array<i16, world::CHUNK_SIZE * world::CHUNK_SIZE> ground;
        std::array<u8, world::CHUNK_SIZE * world::CHUNK_SIZE> wet, nearW;
        world::FeatureContext ctx{ &gen, seed, cols.data() };
        world::applyRivers(*chunk, ctx, ground.data(), wet.data(), nearW.data());
        riverMs += std::chrono::duration<f64, std::milli>(
            std::chrono::steady_clock::now() - a).count();
    }
    std::printf("rivercheck: реки в речном чанке — %.3f мс (%zu чанков)\n",
                riverMs / (f64)wanted.size(), wanted.size());
}

} // namespace

int main() {
    constexpr u64 SEEDS[] = { 0xC0FFEEULL, 0x5A4A9ULL, 0x20260913ULL };

    Stats st;
    for (u64 seed : SEEDS)
        if (!checkWorld(seed, st)) return 1;

    std::printf("rivercheck: рёбер %d, цепочек %d: до моря %d, за пределы %d, "
                "тупиков %d, петель %d\n",
                st.paths, st.chains, st.reachedSea, st.leftArea, st.deadEnds, st.loops);
    std::printf("rivercheck: порядок до %u, узлов озёр %d, рукавов дельт %d, "
                "водопадов %d, проб в протоках %d\n",
                (unsigned)st.maxOrder, st.lakes, st.deltas, st.falls, st.braided);

    bool ok = true;
    auto fail = [&](const char* what) { std::fprintf(stderr, "rivercheck: %s\n", what); ok = false; };
    if (st.rises)      fail("уровень воды поднимается вниз по течению");
    if (st.flowDrops)  fail("сток убывает вниз по течению");
    if (st.seamBreaks) fail("русло рвётся на стыке рёбер или плиток");
    if (st.deadEnds)   fail("есть русла, обрывающиеся на суше");
    if (st.loops)      fail("есть петли в графе стока");
    if (st.reachedSea < st.chains / 2) fail("слишком мало русел доходит до моря");
    if (st.maxOrder < 3) fail("нет притоков третьего порядка");
    if (st.lakes == 0) fail("нет ни одного озера");
    if (st.paths < 1000) fail("сеть слишком редкая");
    if (!ok) return 1;

    timeChunks(0xC0FFEEULL);
    std::printf("rivercheck: PASS\n");
    return 0;
}
