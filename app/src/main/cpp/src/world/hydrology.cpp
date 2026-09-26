/**
 * @file hydrology.cpp
 * @brief Мир: гидрология — водосборы, накопление стока, реки, озёра.
 *
 * Порядок расчёта плитки:
 *   высоты узлов → дождь → заливка впадин → направления стока →
 *   накопление стока → озёра → уровни воды → положения узлов →
 *   подробные русла ядра → индекс сегментов по чанкам.
 * Подробности — в hydrology.h.
 */
#include "hydrology.h"
#include "terrain.h"
#include "chunk.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace world {

static_assert(CHUNK_SIZE == hydro::NODE,
              "озёра интерполируются по четырём узлам вокруг чанка");

u8 packWaterFlow(f32 dirX, f32 dirZ, u32 speedQ, u32 turbulence) {
    const f32 len2 = dirX * dirX + dirZ * dirZ;
    if (len2 < 1e-8f || speedQ == 0) return 0;
    // Румбы как у FLOW_DIR в voxel.frag: 0 — +X, 2 — +Z, 6 — -Z.
    constexpr f32 OCTANT = 0.78539816f;
    i32 oct = (i32)std::lround(std::atan2(dirZ, dirX) / OCTANT);
    oct = ((oct % 8) + 8) % 8;
    if (speedQ > 7) speedQ = 7;
    if (turbulence > 3) turbulence = 3;
    return (u8)((u32)oct | (speedQ << 3) | (turbulence << 6));
}

namespace hydro {

namespace {

constexpr i32 SEA = TerrainGenerator::SEA_LEVEL;
constexpr f32 INF = std::numeric_limits<f32>::infinity();
constexpr f32 TWO_PI = 6.28318531f;

/// Сток, с которого устье разбивается на рукава дельты.
constexpr f32 DELTA_FLOW = 250.f;

/// Сколько плиток держит кэш. Одна плитка — сотни килобайт; игроку
/// нужны от одной до четырёх, остальное — запас на разворот.
constexpr usize TILE_CACHE_MAX   = 12;
constexpr usize HEIGHT_CACHE_MAX = 160;

constexpr i32 D8X[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
constexpr i32 D8Z[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
constexpr f32 D8LEN[8] = { 1.f, 1.41421356f, 1.f, 1.41421356f,
                           1.f, 1.41421356f, 1.f, 1.41421356f };

std::atomic<u32> gNextId{1};

u64 mix64(u64 h) {
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return h;
}

u32 hash2(i32 x, i32 z, u64 salt) {
    return (u32)mix64((u64)(u32)x * 0x9E3779B97F4A7C15ULL ^
                      (u64)(u32)z * 0xC2B2AE3D27D4EB4FULL ^ salt);
}

f32 hash01(i32 x, i32 z, u64 salt) {
    return (f32)(hash2(x, z, salt) >> 8) * (1.f / 16777216.f);
}

u64 key2(i32 a, i32 b) { return ((u64)(u32)a << 32) | (u32)b; }

i32 floorDiv(i32 a, i32 b) {
    const i32 q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

f32 smoothstep(f32 e0, f32 e1, f32 x) {
    f32 t = (x - e0) / (e1 - e0);
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

/// Гладкий шум значений на решётке: для порогов берега и участков
/// разветвлённого русла. Дёшев и детерминирован.
f32 valueNoise(f32 x, f32 z, u64 salt) {
    const f32 fx = std::floor(x), fz = std::floor(z);
    const i32 ix = (i32)fx, iz = (i32)fz;
    f32 tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.f - 2.f * tx);
    tz = tz * tz * (3.f - 2.f * tz);
    const f32 a = hash01(ix, iz, salt),     b = hash01(ix + 1, iz, salt);
    const f32 c = hash01(ix, iz + 1, salt), d = hash01(ix + 1, iz + 1, salt);
    return lerp(lerp(a, b, tx), lerp(c, d, tx), tz);
}

struct V2 { f32 x, z; };

V2 operator+(V2 a, V2 b) { return { a.x + b.x, a.z + b.z }; }
V2 operator-(V2 a, V2 b) { return { a.x - b.x, a.z - b.z }; }
V2 operator*(V2 a, f32 s) { return { a.x * s, a.z * s }; }
f32 length(V2 a) { return std::sqrt(a.x * a.x + a.z * a.z); }

/// Центростремительный Катмулл-Ром: не даёт петель и заострений,
/// когда узлы после сдвига к тальвегу оказались на разных расстояниях.
V2 catmullRom(V2 p0, V2 p1, V2 p2, V2 p3, f32 t) {
    auto knot = [](f32 ti, V2 a, V2 b) {
        return ti + std::max(1e-3f, std::sqrt(length(b - a)));
    };
    const f32 t0 = 0.f;
    const f32 t1 = knot(t0, p0, p1);
    const f32 t2 = knot(t1, p1, p2);
    const f32 t3 = knot(t2, p2, p3);
    const f32 tt = lerp(t1, t2, t);

    const V2 a1 = p0 * ((t1 - tt) / (t1 - t0)) + p1 * ((tt - t0) / (t1 - t0));
    const V2 a2 = p1 * ((t2 - tt) / (t2 - t1)) + p2 * ((tt - t1) / (t2 - t1));
    const V2 a3 = p2 * ((t3 - tt) / (t3 - t2)) + p3 * ((tt - t2) / (t3 - t2));
    const V2 b1 = a1 * ((t2 - tt) / (t2 - t0)) + a2 * ((tt - t0) / (t2 - t0));
    const V2 b2 = a2 * ((t3 - tt) / (t3 - t1)) + a3 * ((tt - t1) / (t3 - t1));
    return b1 * ((t2 - tt) / (t2 - t1)) + b2 * ((tt - t1) / (t2 - t1));
}

f32 depthForHalfWidth(f32 hw) {
    return std::clamp(1.2f + hw * 0.34f, 1.f, 6.f);
}

} // namespace

i32 tileOfNode(i32 n) { return floorDiv(n + TILE_OFFSET_NODES, TILE_NODES); }
i32 tileOfBlock(i32 b) { return floorDiv(b + TILE_OFFSET_NODES * NODE, TILE_BLOCKS); }

namespace {

/// Ключ чанка внутри плитки: запас в восемь чанков с каждой стороны
/// покрывает MAX_REACH.
constexpr i32 CHUNK_KEY_MARGIN = 8;
static_assert(CHUNK_KEY_MARGIN * 32 >= MAX_REACH + 64, "запас ключа покрывает влияние");

u32 chunkKeyIn(i32 tx, i32 tz, i32 cx, i32 cz) {
    const i32 rx = cx - tileFirstNode(tx) + CHUNK_KEY_MARGIN;
    const i32 rz = cz - tileFirstNode(tz) + CHUNK_KEY_MARGIN;
    return ((u32)(rx & 0xFFFF) << 16) | (u32)(rz & 0xFFFF);
}

} // namespace

f32 halfWidthForFlow(f32 flow) {
    if (flow <= 0.f) return 0.f;
    return std::min(14.f, 0.55f + 0.21f * std::sqrt(flow));
}

// ============================================================
// Кэш высот узлов
// ============================================================
//
// Узел — одна колонка генератора в точке (i*NODE, j*NODE). Плитки
// высот мельче гидро-плиток и общие для соседних окон: окно шириной
// 6144 блока при сдвиге на соседнюю плитку досчитывает лишь полосу.

struct Hydrology::HeightTile {
    static constexpr i32 N = HT_NODES * HT_NODES;
    i32 hx = 0, hz = 0;
    f32 hf[N];        ///< высота без округления: у равнин нет ступенек
    i16 hs[N];        ///< высота поверхности, как у генератора
    f32 humid[N];
    f32 cont[N];      ///< континентальность: море — только там, где она < 0
    u8  biome[N];
    u8  lava[N];

    // Сдвиг речного узла к тальвегу. Считается лениво и один раз на
    // узел: речных узлов в окне тысячи, а соседние окна их делят.
    mutable std::atomic<u8> snapState[N];   ///< 0 нет, 1 считается, 2 готово
    mutable i8  snapDx[N];
    mutable i8  snapDz[N];
    mutable i16 snapT[N];
};

struct Hydrology::HeightSlot {
    std::once_flag once;
    std::shared_ptr<const HeightTile> tile;
    u64 used = 0;
};

struct Hydrology::TileSlot {
    std::once_flag once;
    std::shared_ptr<const Tile> tile;
    std::atomic<bool> ready{false};
    u64 used = 0;
};

Hydrology::Hydrology(const TerrainGenerator& terrain, u64 seed)
    : terrain_(terrain),
      id_(gNextId.fetch_add(1, std::memory_order_relaxed))
{
    // Господствующий ветер — свой у каждого мира. От него зависят
    // наветренные склоны (дождливее) и дождевая тень за хребтом.
    const u32 w = (u32)(mix64(seed ^ 0x77D1ULL) & 7u);
    windDx_ = D8X[w];
    windDz_ = D8Z[w];
}

Hydrology::~Hydrology() = default;

std::shared_ptr<const Hydrology::HeightTile>
Hydrology::heightTile(i32 hx, i32 hz) const
{
    std::shared_ptr<HeightSlot> slot;
    {
        std::lock_guard lk(heightMtx_);
        auto& s = heights_[key2(hx, hz)];
        if (!s) s = std::make_shared<HeightSlot>();
        s->used = ++heightClock_;
        slot = s;

        if (heights_.size() > HEIGHT_CACHE_MAX) {
            auto victim = heights_.end();
            for (auto it = heights_.begin(); it != heights_.end(); ++it)
                if (it->second != slot &&
                    (victim == heights_.end() || it->second->used < victim->second->used))
                    victim = it;
            if (victim != heights_.end()) heights_.erase(victim);
        }
    }

    std::call_once(slot->once, [&] {
        auto t = std::make_shared<HeightTile>();
        t->hx = hx; t->hz = hz;
        for (i32 lj = 0; lj < HT_NODES; ++lj)
            for (i32 li = 0; li < HT_NODES; ++li) {
                const i32 k = lj * HT_NODES + li;
                const i32 x = (hx * HT_NODES + li) * NODE;
                const i32 z = (hz * HT_NODES + lj) * NODE;
                const TerrainGenerator::Column col = terrain_.column(x, z);
                // Сток — по крупному рельефу (heightRoute), без холмов:
                // иначе каждая ямка между ними становилась озером.
                t->hf[k] = col.heightRoute;
                t->hs[k] = (i16)col.surface;
                t->humid[k] = col.climate.humidity;
                t->cont[k] = col.climate.continent;
                t->biome[k] = (u8)col.climate.biome;
                t->lava[k] = col.lavaTop > 0 ? 1 : 0;
                t->snapState[k].store(0, std::memory_order_relaxed);
            }
        slot->tile = std::move(t);
    });
    return slot->tile;
}

// ============================================================
// Плитка
// ============================================================

std::shared_ptr<const Tile> Hydrology::tile(i32 tx, i32 tz) const {
    std::shared_ptr<TileSlot> slot;
    {
        std::lock_guard lk(tileMtx_);
        auto& s = tiles_[key2(tx, tz)];
        if (!s) s = std::make_shared<TileSlot>();
        s->used = ++tileClock_;
        slot = s;

        if (tiles_.size() > TILE_CACHE_MAX) {
            auto victim = tiles_.end();
            for (auto it = tiles_.begin(); it != tiles_.end(); ++it)
                if (it->second != slot &&
                    (victim == tiles_.end() || it->second->used < victim->second->used))
                    victim = it;
            if (victim != tiles_.end()) tiles_.erase(victim);
        }
    }

    // Плитки ещё нет — помогаем её строить. Строит одна задача, а
    // остальные, ждущие ту же плитку, иначе стояли бы без дела: пусть
    // заранее посчитают её плитки высот, каждая начиная со своей.
    // Высоты — две трети цены плитки, и на четырёх ядрах первая
    // плитка у точки появления готова втрое быстрее.
    if (!slot->ready.load(std::memory_order_acquire)) {
        static std::atomic<u32> threadSeq{0};
        static thread_local const u32 me = threadSeq.fetch_add(1, std::memory_order_relaxed);
        constexpr i32 HTW = WIN_NODES / HT_NODES;
        const i32 hx0 = floorDiv(tileFirstNode(tx) - APRON_NODES, HT_NODES);
        const i32 hz0 = floorDiv(tileFirstNode(tz) - APRON_NODES, HT_NODES);
        const u32 start = (me * 11u) % (u32)(HTW * HTW);
        for (u32 n = 0; n < (u32)(HTW * HTW); ++n) {
            if (slot->ready.load(std::memory_order_acquire)) break;
            const u32 idx = (start + n) % (u32)(HTW * HTW);
            (void)heightTile(hx0 + (i32)(idx % HTW), hz0 + (i32)(idx / HTW));
        }
    }

    std::call_once(slot->once, [&] {
        slot->tile = build(tx, tz);
        tilesBuilt_.fetch_add(1, std::memory_order_relaxed);
        slot->ready.store(true, std::memory_order_release);
    });
    return slot->tile;
}

bool Hydrology::tileReady(i32 tx, i32 tz) const {
    std::lock_guard lk(tileMtx_);
    const auto it = tiles_.find(key2(tx, tz));
    return it != tiles_.end() && it->second->ready.load(std::memory_order_acquire);
}

std::shared_ptr<Tile> Hydrology::build(i32 tx, i32 tz) const {
    constexpr i32 W = WIN_NODES;
    constexpr i32 NW = W * W;
    const i32 wi0 = tileFirstNode(tx) - APRON_NODES;
    const i32 wj0 = tileFirstNode(tz) - APRON_NODES;

    // ---- высоты окна ----
    constexpr i32 HTW = W / HT_NODES;
    static_assert(W % HT_NODES == 0 && TILE_NODES % HT_NODES == 0 &&
                  APRON_NODES % HT_NODES == 0,
                  "окно собирается из целых плиток высот");
    const i32 hx0 = floorDiv(wi0, HT_NODES);
    const i32 hz0 = floorDiv(wj0, HT_NODES);
    std::vector<std::shared_ptr<const HeightTile>> hts((usize)HTW * HTW);
    for (i32 b = 0; b < HTW; ++b)
        for (i32 a = 0; a < HTW; ++a)
            hts[(usize)b * HTW + a] = heightTile(hx0 + a, hz0 + b);

    static thread_local std::vector<f32> hf, humid, rain, F, fd, Q, dist, phase, flat;
    static thread_local std::vector<i16> hs, L, localL;
    static thread_local std::vector<u8>  biome, lava, seaF;
    static thread_local std::vector<i8>  par;
    static thread_local std::vector<i32> mainChild, lakeOf, order;
    static thread_local std::vector<u8>  strahler, maxOrd, cntOrd, river;
    static thread_local std::vector<i16> minChildL;

    hf.resize(NW); humid.resize(NW); rain.resize(NW);
    hs.resize(NW); biome.resize(NW); lava.resize(NW); seaF.resize(NW);

    auto htOf = [&](i32 li, i32 lj) -> const HeightTile& {
        return *hts[(usize)(lj / HT_NODES) * HTW + (li / HT_NODES)];
    };

    for (i32 lj = 0; lj < W; ++lj)
        for (i32 li = 0; li < W; ++li) {
            const HeightTile& ht = htOf(li, lj);
            const i32 hk = (lj % HT_NODES) * HT_NODES + (li % HT_NODES);
            const i32 k = lj * W + li;
            hf[k] = ht.hf[hk];
            hs[k] = ht.hs[hk];
            humid[k] = ht.humid[hk];
            biome[k] = ht.biome[hk];
            lava[k] = ht.lava[hk];
            // Море — вода на уровне моря в океанической части мира.
            // Лужа на уровне моря посреди материка морем не считается:
            // иначе в пруд шириной в тридцать блоков уходили две
            // большие реки. Такая лужа — обычная впадина, её
            // заливает озеро, и вода переливается дальше к морю.
            seaF[k] = (ht.hs[hk] <= SEA && ht.cont[hk] < 0.f) ? 1 : 0;
        }

    auto inWin = [](i32 li, i32 lj) {
        return (u32)li < (u32)W && (u32)lj < (u32)W;
    };

    // Высота любого узла: из окна, а вне его — из кэша. Дождь узла
    // обязан не зависеть от того, какое окно его считает.
    auto heightAt = [&](i32 li, i32 lj) -> f32 {
        if (inWin(li, lj)) return hf[(usize)lj * W + li];
        const i32 gi = wi0 + li, gj = wj0 + lj;
        const auto ht = heightTile(floorDiv(gi, HT_NODES), floorDiv(gj, HT_NODES));
        const i32 hk = (gj - floorDiv(gj, HT_NODES) * HT_NODES) * HT_NODES +
                       (gi - floorDiv(gi, HT_NODES) * HT_NODES);
        return ht->hf[hk];
    };

    // ---- дождь ----
    //
    // Влажность климата — основа. Сверху: наветренный склон ловит
    // больше осадков, за хребтом — дождевая тень, высокогорье
    // дождливее равнины. Поэтому реки одной стороны хребта крупнее.
    for (i32 lj = 0; lj < W; ++lj)
        for (i32 li = 0; li < W; ++li) {
            const i32 k = lj * W + li;
            if (seaF[k]) { rain[k] = 0.f; continue; }
            const f32 wet = std::clamp((humid[k] + 0.55f) / 1.3f, 0.f, 1.f);
            f32 r = 0.18f + 1.05f * std::pow(wet, 1.4f);
            const f32 up2 = heightAt(li - windDx_ * 2, lj - windDz_ * 2);
            const f32 up5 = heightAt(li - windDx_ * 5, lj - windDz_ * 5);
            const f32 windward = std::clamp((hf[k] - up2) / 24.f, 0.f, 1.f) * 0.6f;
            const f32 shadow = std::clamp((std::max(up2, up5) - hf[k] - 6.f) / 30.f,
                                          0.f, 1.f) * 0.55f;
            r *= (1.f + windward) * (1.f - shadow);
            r += 0.25f * std::clamp((hf[k] - 50.f) / 40.f, 0.f, 1.f);
            rain[k] = r;
        }

    // ---- заливка впадин ----
    //
    // F — высота, до которой узел пришлось бы залить, чтобы вода
    // ушла в море: минимум по всем путям к морю от максимума высоты
    // на пути. Для узла на склоне F равна высоте, во впадине — выше.
    // Значение единственно и не зависит от порядка обхода.
    F.assign(NW, INF);
    {
        using Item = std::pair<f32, i32>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        for (i32 k = 0; k < NW; ++k) {
            if (!seaF[k]) continue;
            F[k] = (f32)SEA;
            const i32 li = k % W, lj = k / W;
            for (i32 d = 0; d < 8; ++d) {
                const i32 ni = li + D8X[d], nj = lj + D8Z[d];
                if (inWin(ni, nj) && !seaF[nj * W + ni]) { pq.push({ (f32)SEA, k }); break; }
            }
        }
        while (!pq.empty()) {
            const auto [f, k] = pq.top();
            pq.pop();
            if (f > F[k]) continue;
            const i32 li = k % W, lj = k / W;
            for (i32 d = 0; d < 8; ++d) {
                const i32 ni = li + D8X[d], nj = lj + D8Z[d];
                if (!inWin(ni, nj)) continue;
                const i32 m = nj * W + ni;
                if (seaF[m]) continue;
                const f32 nf = std::max(hf[m], f);
                if (nf < F[m]) { F[m] = nf; pq.push({ nf, m }); }
            }
        }
    }

    // ---- направления стока ----
    //
    // Узел со склоном стекает по наибольшему уклону F. Узел на
    // ровном месте (залитая впадина) — к выходу с этого ровного места
    // по самому низкому дну: путь ищется Дейкстрой, и шаг по глубокой
    // части впадины дешевле шага по мели. Без этого река пересекала
    // мелкую впадину прямой, как по линейке.
    //
    // Выбор определяется только F, высотами и расстояниями fd, то
    // есть одинаков в любом окне, где впадина поместилась целиком.
    par.assign(NW, (i8)-1);
    fd.assign(NW, INF);
    {
        auto flatCost = [&](i32 m, i32 d) {
            return D8LEN[d] * (0.3f + 1.f / (1.f + std::max(0.f, F[m] - hf[m])));
        };
        using Item = std::pair<f32, i32>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        for (i32 k = 0; k < NW; ++k) {
            if (seaF[k] || F[k] == INF) continue;
            const i32 li = k % W, lj = k / W;
            i32 best = -1;
            f32 bestDrop = 0.f;
            for (i32 d = 0; d < 8; ++d) {
                const i32 ni = li + D8X[d], nj = lj + D8Z[d];
                if (!inWin(ni, nj)) continue;
                const f32 fm = F[nj * W + ni];
                if (!(fm < F[k])) continue;
                const f32 drop = (F[k] - fm) / D8LEN[d];
                if (best < 0 || drop > bestDrop) { best = d; bestDrop = drop; }
            }
            if (best >= 0) {
                par[k] = (i8)best;
                fd[k] = 0.f;
                pq.push({ 0.f, k });
            }
        }
        while (!pq.empty()) {
            const auto [dk, k] = pq.top();
            pq.pop();
            if (dk > fd[k]) continue;
            const i32 li = k % W, lj = k / W;
            for (i32 d = 0; d < 8; ++d) {
                const i32 ni = li + D8X[d], nj = lj + D8Z[d];
                if (!inWin(ni, nj)) continue;
                const i32 m = nj * W + ni;
                if (seaF[m] || F[m] != F[k] || par[m] >= 0) continue;
                const f32 nd = dk + flatCost(m, d);
                if (nd < fd[m]) { fd[m] = nd; pq.push({ nd, m }); }
            }
        }
        for (i32 k = 0; k < NW; ++k) {
            if (seaF[k] || par[k] >= 0 || fd[k] == INF) continue;
            const i32 li = k % W, lj = k / W;
            i32 best = -1;
            f32 bestD = INF;
            for (i32 d = 0; d < 8; ++d) {
                const i32 ni = li + D8X[d], nj = lj + D8Z[d];
                if (!inWin(ni, nj)) continue;
                const i32 m = nj * W + ni;
                if (F[m] != F[k] || !(fd[m] < fd[k])) continue;
                const f32 via = fd[m] + flatCost(k, d);
                if (via < bestD) { bestD = via; best = d; }
            }
            if (best >= 0) par[k] = (i8)best;
        }
    }

    auto parentOf = [&](i32 k) -> i32 {
        if (par[k] < 0) return -1;
        const i32 li = k % W + D8X[par[k]], lj = k / W + D8Z[par[k]];
        return lj * W + li;
    };

    // ---- порядок: притоки раньше приёмников ----
    order.clear();
    for (i32 k = 0; k < NW; ++k)
        if (!seaF[k] && par[k] >= 0) order.push_back(k);
    std::sort(order.begin(), order.end(), [&](i32 a, i32 b) {
        if (F[a] != F[b]) return F[a] > F[b];
        if (fd[a] != fd[b]) return fd[a] > fd[b];
        return a < b;
    });

    // ---- накопление стока ----
    Q.assign(NW, 0.f);
    for (i32 k : order) Q[k] = rain[k];
    mainChild.assign(NW, -1);
    for (i32 k : order) {
        const i32 p = parentOf(k);
        if (p < 0 || seaF[p]) continue;
        Q[p] += Q[k];
    }
    // Русло начинается там, где сток превысил порог. В горах порог
    // ниже: склоны короткие, и без этого у хребта не было бы ни одного
    // ручья. Река не прерывается вниз по течению: если русло есть у
    // притока, оно есть и у приёмника.
    river.assign(NW, 0);
    for (i32 k : order) {
        const f32 thr = RIVER_FLOW * lerp(1.f, 0.4f, smoothstep(45.f, 75.f, hf[k]));
        if (Q[k] >= thr) river[k] = 1;
        const i32 p = parentOf(k);
        if (river[k] && p >= 0 && !seaF[p]) river[p] = 1;
    }

    // Главный приток — самый полноводный речной; при равенстве —
    // меньший индекс, чтобы выбор не зависел от порядка обхода.
    // Порядок по Стралеру: два притока равного порядка дают порядок
    // на единицу больше.
    strahler.assign(NW, 0);
    maxOrd.assign(NW, 0);
    cntOrd.assign(NW, 0);
    for (i32 k : order) {
        if (!river[k]) continue;
        strahler[k] = maxOrd[k] == 0 ? 1
                    : (u8)(cntOrd[k] >= 2 ? maxOrd[k] + 1 : maxOrd[k]);
        const i32 p = parentOf(k);
        if (p < 0 || seaF[p]) continue;
        const i32 mc = mainChild[p];
        if (mc < 0 || Q[k] > Q[mc] || (Q[k] == Q[mc] && k < mc)) mainChild[p] = k;
        if (strahler[k] > maxOrd[p]) { maxOrd[p] = strahler[k]; cntOrd[p] = 1; }
        else if (strahler[k] == maxOrd[p]) ++cntOrd[p];
    }

    // ---- озёра ----
    //
    // Залитая впадина (F выше высоты), в которую приходит река,
    // становится озером с уровнем по порогу. Из озера вода уходит
    // дальше по руслу — сток обязателен, и главный ствол всё равно
    // доходит до моря.
    lakeOf.assign(NW, -1);
    std::vector<i16> lakeLevel;
    std::vector<u8>  lakeFrozen;
    {
        std::vector<i32> comp;
        std::vector<u8> seen(NW, 0);
        for (i32 k0 = 0; k0 < NW; ++k0) {
            if (seen[k0] || seaF[k0] || F[k0] == INF) continue;
            if (F[k0] - hf[k0] < 0.75f) continue;
            comp.clear();
            comp.push_back(k0);
            seen[k0] = 1;
            f32 minH = hf[k0], maxQ = 0.f, minSwamp = INF;
            bool bad = false, cold = false;
            for (usize qi = 0; qi < comp.size(); ++qi) {
                const i32 k = comp[qi];
                minH = std::min(minH, hf[k]);
                // Граница биома тоньше сетки: узел рядом с болотом
                // считается болотным, иначе озеро соседнего узла
                // заливало край болота целиком.
                bool swampy = biome[k] == Swamp;
                for (i32 d = 0; d < 8 && !swampy; ++d) {
                    const i32 ni = k % W + D8X[d], nj = k / W + D8Z[d];
                    swampy = inWin(ni, nj) && biome[nj * W + ni] == Swamp;
                }
                if (swampy) minSwamp = std::min(minSwamp, hf[k]);
                maxQ = std::max(maxQ, Q[k]);
                if (lava[k] || biome[k] == Volcanic) bad = true;
                if (biome[k] == Tundra) cold = true;
                const i32 li = k % W, lj = k / W;
                if (li == 0 || lj == 0 || li == W - 1 || lj == W - 1) bad = true;
                for (i32 d = 0; d < 8; ++d) {
                    const i32 ni = li + D8X[d], nj = lj + D8Z[d];
                    if (!inWin(ni, nj)) continue;
                    const i32 m = nj * W + ni;
                    if (seen[m] || seaF[m] || F[m] != F[k0]) continue;
                    if (F[m] - hf[m] < 0.75f) continue;
                    seen[m] = 1;
                    comp.push_back(m);
                }
            }
            if (bad) continue;
            if (F[k0] - minH < 2.f) continue;
            if (maxQ < RIVER_FLOW * 0.6f) continue;
            // Уровень — порог впадины, но не глубже MAX_LAKE_DEPTH над
            // её дном. Иначе большая пологая впадина становилась
            // «великим озером» в сотни узлов, топившим лес и равнину
            // на пятнадцать блоков; теперь вода стоит в самой низкой
            // её части, а река уходит из озера долиной сквозь порог.
            //
            // Болото — не озеро: вода в нём стоит бочагами по колено,
            // и по нему ходят. Если болото попало во впадину, вода
            // поднимается над самым низким его местом не выше, чем на
            // блок.
            constexpr i32 MAX_LAKE_DEPTH = 6;
            i32 level = (i32)std::floor(F[k0]) - 1;
            level = std::min(level, (i32)std::floor(minH) + MAX_LAKE_DEPTH);
            if (minSwamp < INF)
                level = std::min(level, (i32)std::floor(minSwamp) + 1);
            level = std::max(level, SEA);
            const i32 id = (i32)lakeLevel.size();
            lakeLevel.push_back((i16)level);
            lakeFrozen.push_back(cold ? 1 : 0);
            for (i32 k : comp) lakeOf[k] = id;
        }
    }

    // Озеро ниже по течению не выше озера, из которого в него течёт
    // вода. Уровни с ограничением глубины это не гарантируют: две
    // половины одной котловины получают каждая свой уровень, и
    // нижняя могла оказаться выше верхней — тогда подпор поднимал
    // верхнюю до уровня нижней. Цепочки озёр короткие, поэтому
    // хватает нескольких проходов.
    {
        std::vector<i16> childMin(NW);
        for (i32 iter = 0; iter < 8; ++iter) {
            bool changed = false;
            std::fill(childMin.begin(), childMin.end(), std::numeric_limits<i16>::max());
            for (i32 k : order) {
                i16 v = childMin[k];
                if (lakeOf[k] >= 0) {
                    i16& lv = lakeLevel[(usize)lakeOf[k]];
                    if (childMin[k] < lv) { lv = childMin[k]; changed = true; }
                    v = std::min(v, lv);
                }
                const i32 p = parentOf(k);
                if (p >= 0 && !seaF[p] && v < childMin[p]) childMin[p] = v;
            }
            if (!changed) break;
        }
    }

    // ---- тальвег: сдвиг речного узла к низу долины ----
    static thread_local std::vector<i16> snapT;
    static thread_local std::vector<i8> snapDx, snapDz;
    snapT.assign(NW, 0);
    snapDx.assign(NW, 0);
    snapDz.assign(NW, 0);
    for (i32 k : order) {
        if (!river[k]) continue;
        const i32 li = k % W, lj = k / W;
        const HeightTile& ht = htOf(li, lj);
        const i32 hk = (lj % HT_NODES) * HT_NODES + (li % HT_NODES);
        u8 st = ht.snapState[hk].load(std::memory_order_acquire);
        if (st == 2) {
            snapT[k] = ht.snapT[hk];
            snapDx[k] = ht.snapDx[hk];
            snapDz[k] = ht.snapDz[hk];
            continue;
        }
        const i32 gx = (wi0 + li) * NODE, gz = (wj0 + lj) * NODE;
        constexpr i32 OFF = 11;
        const i32 cand[5][2] = { {0, 0}, {OFF, 0}, {-OFF, 0}, {0, OFF}, {0, -OFF} };
        i32 bestT = hs[k], bdx = 0, bdz = 0;
        for (i32 c = 1; c < 5; ++c) {
            const i32 t = terrain_.surfaceHeight(gx + cand[c][0], gz + cand[c][1]);
            if (t < bestT) { bestT = t; bdx = cand[c][0]; bdz = cand[c][1]; }
        }
        snapT[k] = (i16)bestT;
        snapDx[k] = (i8)bdx;
        snapDz[k] = (i8)bdz;
        u8 expected = 0;
        if (ht.snapState[hk].compare_exchange_strong(expected, 1,
                                                     std::memory_order_acq_rel)) {
            ht.snapT[hk] = (i16)bestT;
            ht.snapDx[hk] = (i8)bdx;
            ht.snapDz[hk] = (i8)bdz;
            ht.snapState[hk].store(2, std::memory_order_release);
        }
    }

    // ---- уровни воды ----
    //
    // Вода не поднимается вниз по течению: уровень речного узла —
    // минимум из своего (на два блока ниже тальвега) и уровней всех
    // притоков. Озеро держит уровень своего порога. Если приток
    // подходит к озеру ниже его зеркала, озеро подпирает его: такой
    // узел поднимается до уровня озера и становится его заливом —
    // иначе вода в устье притока стояла бы ниже воды, в которую он
    // впадает.
    L.assign(NW, (i16)-1);
    localL.assign(NW, (i16)-1);
    minChildL.assign(NW, std::numeric_limits<i16>::max());
    for (i32 k : order) {
        if (lakeOf[k] >= 0) localL[k] = lakeLevel[(usize)lakeOf[k]];
        else if (river[k]) {
            const f32 hw = halfWidthForFlow(Q[k]);
            localL[k] = (i16)(snapT[k] - 2 - (hw >= 4.f ? 1 : 0));
        }
    }
    for (i32 k : order) {
        if (!river[k] && lakeOf[k] < 0) continue;
        i32 l = lakeOf[k] >= 0 ? (i32)localL[k]
                               : std::min<i32>(localL[k], minChildL[k]);
        l = std::max(l, SEA);
        L[k] = (i16)l;
        const i32 p = parentOf(k);
        if (p < 0 || seaF[p]) continue;
        if (L[k] < minChildL[p]) minChildL[p] = L[k];
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const i32 k = *it;
        if (!river[k] && lakeOf[k] < 0) continue;
        const i32 p = parentOf(k);
        if (p < 0 || seaF[p] || L[p] < 0) continue;
        if (L[k] < L[p]) {
            L[k] = L[p];
            if (lakeOf[p] >= 0) lakeOf[k] = lakeOf[p];
        }
    }

    // ---- расстояние до устья, фаза меандра, равнинность ----
    dist.assign(NW, 0.f);
    phase.assign(NW, 0.f);
    flat.assign(NW, 0.f);
    static thread_local std::vector<u32> basin;
    basin.assign(NW, 0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const i32 k = *it;
        const i32 p = parentOf(k);
        const f32 len = D8LEN[par[k]] * (f32)NODE;
        const f32 lam = std::clamp(halfWidthForFlow(Q[k]) * 14.f, 60.f, 220.f);
        if (seaF[p]) {
            const i32 gi = wi0 + p % W, gj = wj0 + p / W;
            dist[k] = len;
            phase[k] = hash01(gi, gj, 0x3EA7DULL) + len / lam;
            basin[k] = hash2(wi0 + k % W, wj0 + k / W, 0xBA51ULL) | 1u;
        } else {
            dist[k] = dist[p] + len;
            phase[k] = phase[p] + len / lam;
            basin[k] = basin[p];
        }
        const f32 hp = seaF[p] ? (f32)SEA : hf[p];
        const f32 slope = std::max(0.f, hf[k] - hp) / len;
        flat[k] = 1.f - smoothstep(0.012f, 0.08f, slope);
    }

    // Разветвлённое русло — только у большой реки на плоской равнине,
    // и участками: шум решает, где река разбивается на протоки.
    auto braidOf = [&](i32 k) -> f32 {
        if (k < 0 || seaF[k] || !river[k] || lakeOf[k] >= 0) return 0.f;
        if (halfWidthForFlow(Q[k]) < 4.2f || flat[k] < 0.8f) return 0.f;
        const f32 n = valueNoise((f32)(wi0 + k % W) / 7.f,
                                 (f32)(wj0 + k / W) / 7.f, 0xB4A1DULL);
        return smoothstep(0.52f, 0.62f, n);
    };

    // ---- положение узла ----
    //
    // Горный поток прижимается к тальвегу. Большая равнинная река
    // уходит от него в меандр: плавный сдвиг поперёк течения, фаза
    // которого копится вдоль реки, поэтому изгибы соседних рёбер
    // складываются в одну волну.
    auto gridPos = [&](i32 k) -> V2 {
        return { (f32)((wi0 + k % W) * NODE), (f32)((wj0 + k / W) * NODE) };
    };
    auto posOf = [&](i32 k) -> V2 {
        V2 g = gridPos(k);
        if (seaF[k] || !river[k]) return g;
        // Свой сдвиг у каждого узла: без него ручьи на ровном склоне
        // шли строго по диагоналям сетки.
        const i32 gi = wi0 + k % W, gj = wj0 + k / W;
        g.x += (hash01(gi, gj, 0x71EEULL) - 0.5f) * 12.f;
        g.z += (hash01(gi, gj, 0x72EEULL) - 0.5f) * 12.f;
        const f32 hw = halfWidthForFlow(Q[k]);
        const f32 m = flat[k] * flat[k] * std::clamp(0.35f + hw / 8.f, 0.f, 1.f);
        const i32 p = parentOf(k);
        const i32 c = mainChild[k];
        V2 dir = gridPos(p) - (c >= 0 ? gridPos(c) : g);
        const f32 dl = length(dir);
        if (dl > 1e-3f) dir = dir * (1.f / dl);
        const V2 normal{ -dir.z, dir.x };
        const f32 lam = std::clamp(hw * 14.f, 60.f, 220.f);
        const f32 amp = std::min(0.32f * (f32)NODE, 0.18f * lam);
        const f32 off = amp * std::sin(TWO_PI * phase[k]) * m;
        return { g.x + (f32)snapDx[k] * (1.f - m) + normal.x * off,
                 g.z + (f32)snapDz[k] * (1.f - m) + normal.z * off };
    };

    auto reliefOf = [&](i32 k) -> f32 {
        const i32 li = k % W, lj = k / W;
        f32 top = hf[k];
        for (i32 d = 0; d < 8; ++d) {
            const i32 ni = li + D8X[d], nj = lj + D8Z[d];
            if (inWin(ni, nj)) top = std::max(top, hf[nj * W + ni]);
        }
        return std::max(0.f, top - (f32)L[k]);
    };

    auto tileOut = std::make_shared<Tile>();
    Tile& out = *tileOut;
    out.tx = tx; out.tz = tz;
    constexpr i32 TN = TILE_NODES * TILE_NODES;
    out.nodeFlags.assign(TN, 0);
    out.lakeLevel.assign(TN, -1);
    out.nodeLevel.assign(TN, -1);
    out.nodeFlow.assign(TN, 0.f);
    out.nodeBasin.assign(TN, 0);
    out.nodeOrder.assign(TN, 0);

    for (i32 lj = 0; lj < TILE_NODES; ++lj)
        for (i32 li = 0; li < TILE_NODES; ++li) {
            const i32 k = (lj + APRON_NODES) * W + (li + APRON_NODES);
            const i32 o = lj * TILE_NODES + li;
            u8 f = 0;
            if (seaF[k]) f |= NF_SEA;
            else if (par[k] < 0) f |= NF_UNDRAINED;
            if (river[k]) f |= NF_RIVER;
            if (lakeOf[k] >= 0) {
                f |= NF_LAKE;
                out.lakeLevel[o] = lakeLevel[(usize)lakeOf[k]];
                if (lakeFrozen[(usize)lakeOf[k]]) f |= NF_FROZEN;
            }
            if (biome[k] == Tundra) f |= NF_FROZEN;
            out.nodeFlags[o] = f;
            out.nodeLevel[o] = L[k];
            out.nodeFlow[o] = Q[k];
            out.nodeBasin[o] = basin[k];
            out.nodeOrder[o] = strahler[k];
        }

    // ---- подробные русла рёбер ядра ----
    auto emitSample = [&](Sample s) { out.samples.push_back(s); };

    std::vector<i16> lev;
    std::vector<V2>  pts;
    std::vector<u8>  frozenAt;

    auto finishPath = [&](Path& path, const std::vector<V2>& p,
                          const std::vector<i16>& lv, f32 u0, f32 u1,
                          f32 hwA, f32 hwB, f32 flatA, f32 flatB,
                          f32 reliefA, f32 reliefB, f32 braidA, f32 braidB,
                          bool inLake, bool delta, bool headwater) {
        const i32 n = (i32)p.size() - 1;
        path.first = (u32)out.samples.size();
        path.count = (u32)p.size();
        const f32 spacing = n > 0 ? std::max(1.f, length(p[n] - p[0]) / (f32)n) : 1.f;

        for (i32 s = 0; s <= n; ++s) {
            const f32 t = n > 0 ? (f32)s / (f32)n : 0.f;
            Sample o;
            o.x = p[s].x;
            o.z = p[s].z;
            o.u = lerp(u0, u1, t);
            f32 hw = lerp(hwA, hwB, t);
            if (headwater) hw *= lerp(0.45f, 1.f, smoothstep(0.f, 0.8f, t));
            o.hw = std::max(0.6f, hw);
            o.depth = depthForHalfWidth(o.hw);
            const f32 fl = lerp(flatA, flatB, t);
            o.wall = lerp(1.3f, 0.4f, fl);
            o.bank = o.hw * lerp(0.15f, 1.0f, fl) + lerp(0.5f, 2.5f, fl);
            if (delta) { o.bank = o.hw + 3.f; o.wall = 0.4f; }
            const f32 relief = lerp(reliefA, reliefB, t);
            const f32 valley = std::clamp(relief / o.wall + 2.f, 3.f, 36.f);
            o.reach = std::min(64.f, o.hw + o.bank + valley);
            o.braid = lerp(braidA, braidB, t);
            if (o.braid > 0.f) o.reach = std::min(64.f, o.reach + o.hw * 0.8f);
            o.waterY = lv[s];

            u8 flags = 0;
            if (fl < 0.35f && o.hw < 6.f) flags |= SF_STONE_BED;
            if ((o.hw >= 3.2f && fl > 0.5f) || delta) flags |= SF_SAND_BANK;
            if (delta) flags |= SF_DELTA;
            if (inLake) flags |= SF_IN_LAKE;
            if (frozenAt[s]) flags |= SF_FROZEN;

            // Течение: направление по касательной, скорость по уклону
            // воды на ближайших пробах, бурление — пороги и водопады.
            const i32 s0 = std::max(0, s - 1), s1 = std::min(n, s + 1);
            const V2 tan = p[s1] - p[s0];
            const i32 s2 = std::min(n, s + 2);
            const f32 fall = (f32)(lv[s] - lv[s2]) /
                             std::max(1.f, (f32)(s2 - s) * spacing);
            const f32 speed = std::clamp(0.18f + fall * 5.f + (1.f - fl) * 0.35f
                                         - o.hw * 0.01f, 0.1f, 1.f);
            u32 speedQ = (u32)std::lround(speed * 7.f);
            if (speedQ < 1) speedQ = 1;
            u32 turb = 0;
            if (1.f - fl > 0.55f) turb = 1;
            if (fall > 0.10f) turb = 2;
            const bool fallHere = s > 0 && lv[s - 1] - lv[s] >= 3;
            const bool fallNext = s < n && lv[s] - lv[s + 1] >= 3;
            if (fallHere || fallNext) { turb = 3; speedQ = 7; }
            if (fallHere) flags |= SF_PLUNGE;
            if (inLake) { speedQ = 1; turb = 0; }
            o.flow = packWaterFlow(tan.x, tan.z, speedQ, turb);
            o.flags = flags;
            emitSample(o);
        }
        out.paths.push_back(path);
    };

    for (i32 lj = 0; lj < TILE_NODES; ++lj)
        for (i32 li = 0; li < TILE_NODES; ++li) {
            const i32 k = (lj + APRON_NODES) * W + (li + APRON_NODES);
            if (!river[k] || par[k] < 0) continue;
            const i32 p = parentOf(k);
            const bool pSea = seaF[p] != 0;

            const V2 P1 = posOf(k);
            const V2 P2 = posOf(p);
            const i32 mc = mainChild[k];
            const bool headwater = mc < 0 || !river[mc];
            const V2 P0 = headwater ? P1 + (P1 - P2) : posOf(mc);
            const i32 pp = pSea ? -1 : parentOf(p);
            const V2 P3 = (pp < 0) ? P2 + (P2 - P1) : posOf(pp);

            const i32 Ln = L[k];
            const i32 Lp = pSea ? SEA : L[p];
            // Ребро внутри озера — оба конца в озёрах (не обязательно
            // одном: подпёртый залив и само озеро — разные номера). Его
            // воду держит озеро, и ни долины, ни береговых валов у него
            // нет: иначе посреди озера вставали две дамбы.
            const bool inLake = lakeOf[k] >= 0 && lakeOf[p] >= 0;
            const f32 hwN = halfWidthForFlow(Q[k]);
            const bool mainStem = !pSea && mainChild[p] == k;
            const f32 hwEnd = mainStem ? halfWidthForFlow(Q[p]) : hwN;
            const f32 inc = hwN >= 4.f ? 1.f : 0.f;

            // Ручей-исток петляет мелко, между узлами: сетка его
            // извилин не видит.
            const u32 eh = hash2(wi0 + li + APRON_NODES, wj0 + lj + APRON_NODES, 0x5E6A7ULL);
            const f32 mN = flat[k] * flat[k] * std::clamp(0.35f + hwN / 8.f, 0.f, 1.f);
            const f32 wig = (hwN < 2.6f && strahler[k] <= 2)
                          ? (1.f - 0.5f * mN) * (1.6f + hwN * 0.8f) * ((eh & 1u) ? 1.f : -1.f)
                          : 0.f;
            const f32 wigK = (eh & 2u) ? 2.f : 1.f;

            const f32 len = length(P2 - P1);
            const i32 n = std::max(2, (i32)std::ceil(len / 4.f));
            pts.resize((usize)n + 1);
            lev.resize((usize)n + 1);
            frozenAt.assign((usize)n + 1, 0);
            for (i32 s = 0; s <= n; ++s) {
                const f32 t = (f32)s / (f32)n;
                V2 pt = catmullRom(P0, P1, P2, P3, t);
                if (wig != 0.f) {
                    const V2 a = catmullRom(P0, P1, P2, P3, std::max(0.f, t - 0.02f));
                    const V2 b = catmullRom(P0, P1, P2, P3, std::min(1.f, t + 0.02f));
                    V2 tg = b - a;
                    const f32 tl = length(tg);
                    if (tl > 1e-4f) {
                        tg = tg * (1.f / tl);
                        const f32 off = wig * std::sin(3.14159265f * wigK * t);
                        pt = pt + V2{ -tg.z, tg.x } * off;
                    }
                }
                pts[(usize)s] = pt;
            }

            // Уровень вдоль ребра: следует рельефу вниз, не поднимаясь
            // и не опускаясь ниже приёмника. Где рельеф обрывается —
            // обрывается и вода: там водопад.
            i32 cur = Ln;
            lev[0] = (i16)Ln;
            for (i32 s = 0; s <= n; ++s) {
                const i32 xi = (i32)std::floor(pts[(usize)s].x);
                const i32 zi = (i32)std::floor(pts[(usize)s].z);
                const TerrainGenerator::Column col = terrain_.column(xi, zi);
                frozenAt[(usize)s] = col.climate.biome == Tundra ? 1 : 0;
                if (s == 0) continue;
                if (inLake) { lev[(usize)s] = (i16)Ln; continue; }
                const i32 want = col.surface - 2 - (i32)inc;
                cur = std::max(Lp, std::min(cur, want));
                lev[(usize)s] = (i16)cur;
            }
            lev[(usize)n] = (i16)Lp;
            for (i32 s = n - 1; s >= 1; --s)
                lev[(usize)s] = std::max(lev[(usize)s], lev[(usize)s + 1]);

            Path path;
            path.fromI = wi0 + k % W;  path.fromJ = wj0 + k / W;
            path.toI = wi0 + p % W;    path.toJ = wj0 + p / W;
            path.flow = Q[k];
            path.basin = basin[k];
            path.order = strahler[k];
            path.kind = PATH_RIVER;
            path.mainStem = mainStem;
            path.mouth = pSea;

            const f32 relN = reliefOf(k);
            const f32 relP = pSea ? relN : reliefOf(p);
            const f32 flP = pSea ? flat[k] : flat[p];
            finishPath(path, pts, lev, dist[k], pSea ? 0.f : dist[p],
                       hwN, hwEnd, flat[k], flP, relN, relP,
                       braidOf(k), pSea ? 0.f : braidOf(p),
                       inLake, false, headwater);

            // ---- дельта ----
            //
            // Большая равнинная река у моря расходится рукавами:
            // каждый рукав уже главного русла и сам ищет море. Рукав,
            // не дошедший до воды, не рисуется — тупиковых проток
            // здесь не бывает.
            if (!pSea || Q[k] < DELTA_FLOW || flat[k] < 0.45f) continue;

            // Вершина дельты — на два-три узла выше устья по главному
            // стволу: последний узел суши стоит у самой воды, и рукава
            // от него упирались бы в море через шаг.
            i32 apex = k;
            for (i32 up = 0; up < 3; ++up) {
                const i32 c = mainChild[apex];
                if (c < 0 || !river[c] || lakeOf[c] >= 0) break;
                apex = c;
            }
            if (apex == k) continue;
            const V2 A0 = posOf(apex);
            const i32 La = L[apex];
            const f32 toSea = length(P2 - A0);
            if (toSea < 1e-3f) continue;
            const V2 d0 = (P2 - A0) * (1.f / toSea);
            // Рукава расходятся веером под разными углами; берутся те,
            // что дошли до моря. Берег редко встречает реку ровным
            // фронтом, поэтому углов несколько, а рукавов — два-три.
            const i32 wantArms = Q[k] >= 1000.f ? 3 : 2;
            const f32 side = ((eh >> 3) & 1u) ? 1.f : -1.f;
            const f32 ANGLES[6] = { 0.35f * side, -0.35f * side, 0.65f * side,
                                    -0.65f * side, 0.18f * side, -0.18f * side };
            i32 arms = 0;
            for (i32 a = 0; a < 6 && arms < wantArms; ++a) {
                const f32 ang = ANGLES[a];
                const f32 ca = std::cos(ang), sa = std::sin(ang);
                const V2 dir{ d0.x * ca - d0.z * sa, d0.x * sa + d0.z * ca };
                const f32 reachLen = toSea * 1.25f + (f32)NODE;
                const V2 c1 = A0 + dir * (0.55f * reachLen);
                const V2 e = c1 + d0 * (0.65f * reachLen);
                const i32 STEPS = std::max(12, (i32)std::ceil(reachLen * 1.2f / 3.f));
                pts.clear();
                lev.clear();
                frozenAt.clear();
                i32 levCur = La;
                i32 afterSea = -1;
                // Рукав петляет сам по себе: прямой желоб от вершины до
                // моря читался как канал. Изгиб гаснет к концам, чтобы
                // рукав выходил из реки и входил в море на своём месте.
                const u32 ah = hash2(wi0 + apex % W + a * 7, wj0 + apex / W, 0xDE17AULL);
                const f32 wobA = reachLen * (0.06f + 0.06f * (f32)(ah & 255u) / 255.f)
                               * ((ah & 256u) ? 1.f : -1.f);
                const f32 wobK = (ah & 512u) ? 2.f : 1.f;
                const V2 across{ -dir.z, dir.x };
                for (i32 s = 0; s <= STEPS; ++s) {
                    const f32 t = (f32)s / (f32)STEPS;
                    V2 q = A0 * ((1.f - t) * (1.f - t)) + c1 * (2.f * (1.f - t) * t)
                         + e * (t * t);
                    q = q + across * (wobA * std::sin(3.14159265f * wobK * t) *
                                      std::sin(3.14159265f * t));
                    const TerrainGenerator::Column col =
                        terrain_.column((i32)std::floor(q.x), (i32)std::floor(q.z));
                    if (s > 0) levCur = std::max(SEA, std::min(levCur, col.surface - 2));
                    pts.push_back(q);
                    lev.push_back((i16)levCur);
                    frozenAt.push_back(col.climate.biome == Tundra ? 1 : 0);
                    if (col.surface <= SEA && afterSea < 0) afterSea = s;
                    if (afterSea >= 0 && s >= afterSea + 2) break;
                }
                if (afterSea < 3 || pts.size() < 5) continue;
                lev.back() = (i16)SEA;
                for (i32 s = (i32)lev.size() - 2; s >= 1; --s)
                    lev[(usize)s] = std::max(lev[(usize)s], lev[(usize)s + 1]);

                Path arm = path;
                arm.kind = PATH_DELTA;
                arm.mainStem = false;
                arm.mouth = true;
                arm.fromI = wi0 + apex % W;
                arm.fromJ = wj0 + apex / W;
                const f32 hwArm = std::max(1.2f, hwN * 0.55f);
                finishPath(arm, pts, lev, dist[apex], 0.f, hwArm, hwArm,
                           1.f, 1.f, 0.f, 0.f, 0.f, 0.f, false, true, false);
                ++arms;
            }
        }

    // ---- индекс сегментов по чанкам ----
    for (const Path& path : out.paths) {
        for (u32 s = 0; s + 1 < path.count; ++s) {
            const u32 a = path.first + s;
            const Sample& A = out.samples[a];
            const Sample& B = out.samples[a + 1];
            const f32 r = std::max(A.reach, B.reach) + 1.f;
            Segment seg;
            seg.a = a;
            seg.x0 = std::min(A.x, B.x) - r;
            seg.z0 = std::min(A.z, B.z) - r;
            seg.x1 = std::max(A.x, B.x) + r;
            seg.z1 = std::max(A.z, B.z) + r;
            const u32 id = (u32)out.segs.size();
            out.segs.push_back(seg);

            const i32 c0x = floorDiv((i32)std::floor(seg.x0), CHUNK_SIZE);
            const i32 c1x = floorDiv((i32)std::floor(seg.x1), CHUNK_SIZE);
            const i32 c0z = floorDiv((i32)std::floor(seg.z0), CHUNK_SIZE);
            const i32 c1z = floorDiv((i32)std::floor(seg.z1), CHUNK_SIZE);
            for (i32 cz = c0z; cz <= c1z; ++cz)
                for (i32 cx = c0x; cx <= c1x; ++cx)
                    out.chunkSegs.push_back({ chunkKeyIn(tx, tz, cx, cz), id });
        }
    }
    std::sort(out.chunkSegs.begin(), out.chunkSegs.end());

    out.samples.shrink_to_fit();
    out.paths.shrink_to_fit();
    out.segs.shrink_to_fit();
    out.chunkSegs.shrink_to_fit();
    return tileOut;
}

// ============================================================
// Запросы
// ============================================================

NodeInfo Hydrology::node(i32 i, i32 j) const {
    const i32 tx = tileOfNode(i), tz = tileOfNode(j);
    const auto t = tile(tx, tz);
    const i32 li = i - tileFirstNode(tx), lj = j - tileFirstNode(tz);
    const i32 o = lj * TILE_NODES + li;
    NodeInfo n;
    n.flags = t->nodeFlags[(usize)o];
    n.lakeLevel = t->lakeLevel[(usize)o];
    n.level = t->nodeLevel[(usize)o];
    n.flow = t->nodeFlow[(usize)o];
    n.basin = t->nodeBasin[(usize)o];
    n.order = t->nodeOrder[(usize)o];
    return n;
}

void Hydrology::chunkView(i32 cx, i32 cz, ChunkView& out) const {
    out.cx = cx;
    out.cz = cz;
    out.tiles.clear();
    out.segs.clear();

    const i32 bx0 = cx * CHUNK_SIZE, bz0 = cz * CHUNK_SIZE;
    const i32 tx0 = tileOfBlock(bx0 - MAX_REACH);
    const i32 tx1 = tileOfBlock(bx0 + CHUNK_SIZE - 1 + MAX_REACH);
    const i32 tz0 = tileOfBlock(bz0 - MAX_REACH);
    const i32 tz1 = tileOfBlock(bz0 + CHUNK_SIZE - 1 + MAX_REACH);

    for (i32 tz = tz0; tz <= tz1; ++tz)
        for (i32 tx = tx0; tx <= tx1; ++tx) {
            auto t = tile(tx, tz);
            const u32 ck = chunkKeyIn(tx, tz, cx, cz);
            const auto lo = std::lower_bound(
                t->chunkSegs.begin(), t->chunkSegs.end(),
                std::pair<u32, u32>{ ck, 0u });
            for (auto it = lo; it != t->chunkSegs.end() && it->first == ck; ++it) {
                const Segment& s = t->segs[it->second];
                out.segs.push_back({ &t->samples[s.a], &t->samples[s.a + 1],
                                     s.x0, s.z0, s.x1, s.z1 });
            }
            out.tiles.push_back(std::move(t));
        }

    for (i32 c = 0; c < 4; ++c) {
        const i32 i = cx + (c & 1), j = cz + (c >> 1);
        const i32 tx = tileOfNode(i), tz = tileOfNode(j);
        std::shared_ptr<const Tile> t;
        for (const auto& have : out.tiles)
            if (have->tx == tx && have->tz == tz) { t = have; break; }
        if (!t) { t = tile(tx, tz); out.tiles.push_back(t); }
        const i32 o = (j - tileFirstNode(tz)) * TILE_NODES + (i - tileFirstNode(tx));
        out.lake[c] = t->lakeLevel[(usize)o];
        out.lakeFrozen[c] = (t->nodeFlags[(usize)o] & NF_FROZEN) != 0;
    }
}

ColumnWater Hydrology::evaluate(const ChunkView& v, i32 wx, i32 wz, i32 raw,
                                const ChunkView::Seg* segs, usize count) {
    if (!segs) { segs = v.segs.data(); count = v.segs.size(); }
    ColumnWater r;
    r.surface = raw;
    // Ниже уровня моря вода уже налита applyLiquids: русло не
    // вырезает из неё своего уровня. Озеро над такой лужей — может:
    // его зеркало выше моря.
    const bool lowland = raw <= SEA;

    const f32 px = (f32)wx + 0.5f, pz = (f32)wz + 0.5f;
    // Рваная кромка: ровный по циркулю берег читается как канава.
    const f32 jitter = (hash01(wx, wz, 0x5EEDB4ULL) - 0.5f) * 0.6f;

    bool chan = false;
    i32 chanY = 0;
    f32 chanRel = INF, chanDepth = 1.f;
    u8 chanFlow = 0, chanFlags = 0;

    f32 carve = INF;
    f32 nearMax = 0.f;
    i32 levee = std::numeric_limits<i32>::min();
    f32 groundBest = INF;
    u8 groundFlags = 0;
    bool bar = false, nearWater = false;

    for (usize si = 0; si < count; ++si) {
        const ChunkView::Seg& sg = segs[si];
        if (lowland) break;
        if (px < sg.x0 || px > sg.x1 || pz < sg.z0 || pz > sg.z1) continue;
        const Sample& A = *sg.a;
        const Sample& B = *sg.b;
        const f32 abx = B.x - A.x, abz = B.z - A.z;
        const f32 len2 = abx * abx + abz * abz;
        f32 t = 0.f;
        if (len2 > 1e-6f)
            t = std::clamp(((px - A.x) * abx + (pz - A.z) * abz) / len2, 0.f, 1.f);
        const f32 qx = A.x + abx * t, qz = A.z + abz * t;
        const f32 d = std::sqrt((px - qx) * (px - qx) + (pz - qz) * (pz - qz));
        const f32 reach = lerp(A.reach, B.reach, t);
        if (d > reach) continue;
        {
            // Близость воды: по ширине долины, и большая река сырит
            // берег сильнее ручья.
            const f32 size = std::clamp(lerp(A.hw, B.hw, t) / 6.f, 0.35f, 1.f);
            nearMax = std::max(nearMax, (1.f - d / reach) * size);
        }

        const Sample& N = t < 0.5f ? A : B;
        const i32 y = N.waterY;
        const u8 flags = N.flags;
        const f32 hw = lerp(A.hw, B.hw, t);
        const f32 depth = lerp(A.depth, B.depth, t);
        const f32 bank = lerp(A.bank, B.bank, t);
        const f32 wall = lerp(A.wall, B.wall, t);
        const f32 braid = lerp(A.braid, B.braid, t);
        const f32 de = d + jitter;

        bool inWater = false, isBar = false;
        f32 rel = de / hw, dep = depth, core = hw;
        if (braid > 0.02f) {
            // Протоки плетутся вокруг оси: у каждой свой сдвиг поперёк
            // течения, меняющийся вдоль реки, между ними — косы.
            // Знаковое расстояние до ОТРЕЗКА: до бесконечной прямой
            // за концами отрезка давало лучи во все стороны.
            const f32 cross = abx * (pz - A.z) - abz * (px - A.x);
            const f32 lat = cross >= 0.f ? d : -d;
            const f32 uu = lerp(A.u, B.u, t);
            const f32 lam = std::max(40.f, hw * 9.f);
            const f32 belt = hw * (1.f + 0.8f * braid);
            const f32 sub = hw * (1.f - 0.55f * braid);
            rel = INF;
            for (i32 kk = 0; kk < 3; ++kk) {
                const f32 c = braid * belt * 0.6f *
                              std::sin(uu * TWO_PI / lam + (f32)kk * 2.0944f);
                const f32 e = std::fabs(lat - c) + jitter;
                if (e <= sub) { inWater = true; rel = std::min(rel, e / sub); }
            }
            dep = depth * (1.f - 0.35f * braid);
            core = belt;
            if (!inWater && de <= belt) isBar = true;
        } else {
            inWater = de <= hw;
        }

        if (inWater) {
            if (!chan || y < chanY || (y == chanY && rel < chanRel)) {
                chan = true;
                chanY = y;
                chanRel = rel;
                const f32 rr = std::clamp(rel, 0.f, 1.f);
                chanDepth = dep * (1.f - 0.7f * rr * rr);
                if (flags & SF_PLUNGE) chanDepth += 2.f;
                chanFlow = N.flow;
                chanFlags = flags;
            }
            continue;
        }
        if (flags & SF_IN_LAKE) {
            // Долину в озере не режем, но берег у воды держим: русло,
            // входящее в озеро выше его зеркала, стоит на своём уровне,
            // и берег по уровню озера оставлял воду рядом с воздухом.
            const f32 dd = std::max(0.f, de - core);
            if (dd <= 1.5f) {
                levee = std::max(levee, y + 1);
                nearWater = true;
            }
            continue;
        }

        const f32 dd = std::max(0.f, de - core);
        const f32 base = (isBar || (flags & SF_DELTA)) ? 1.f : 2.f;
        const f32 g = (f32)y + base + std::max(0.f, dd - bank) * wall;
        const f32 fade = smoothstep(reach - 6.f, reach, d);
        carve = std::min(carve, g + ((f32)raw - g) * fade);
        if (dd <= 1.5f) {
            levee = std::max(levee, y + (i32)base);
            nearWater = true;
        }
        if (dd < groundBest) {
            groundBest = dd;
            groundFlags = flags;
            bar = isBar;
        }
    }

    // ---- озёра ----
    //
    // Граница — изолиния по четырём узлам вокруг колонки, а не
    // квадраты сетки. Порог слегка шумит: берег выходит изрезанным.
    bool inLake = false, rim = false, lakeFrozen = false;
    i32 lk = -1;
    if (v.lake[0] >= 0 || v.lake[1] >= 0 || v.lake[2] >= 0 || v.lake[3] >= 0) {
        const f32 fx = ((f32)(wx - v.cx * NODE) + 0.5f) / (f32)NODE;
        const f32 fz = ((f32)(wz - v.cz * NODE) + 0.5f) / (f32)NODE;
        const f32 wgt[4] = { (1.f - fx) * (1.f - fz), fx * (1.f - fz),
                             (1.f - fx) * fz,         fx * fz };
        f32 bestW = 0.f;
        for (i32 c = 0; c < 4; ++c) {
            if (v.lake[c] < 0) continue;
            f32 w = 0.f;
            bool fr = false;
            for (i32 e = 0; e < 4; ++e)
                if (v.lake[e] == v.lake[c]) { w += wgt[e]; fr = fr || v.lakeFrozen[e]; }
            if (w > bestW) { bestW = w; lk = v.lake[c]; lakeFrozen = fr; }
        }
        const f32 thr = 0.5f + (valueNoise((f32)wx / 7.f, (f32)wz / 7.f, 0x1A4EULL) - 0.5f) * 0.24f;
        inLake = lk >= 0 && bestW >= thr;
        rim = lk >= 0 && !inLake && bestW > 0.15f;
    }

    if (inLake) nearMax = 1.f;
    else if (rim) nearMax = std::max(nearMax, 0.8f);
    r.near = nearMax;

    if (lowland) {
        if (inLake && lk > SEA) {
            r.kind = ColumnWater::Lake;
            r.waterTop = lk;
            r.frozen = lakeFrozen;
        } else if (rim && lk > SEA) {
            r.kind = ColumnWater::Ground;
            r.surface = lk + 1;
        }
        return r;
    }

    i32 s = raw;
    if (carve < INF) s = std::min(raw, (i32)std::floor(carve + 0.5f));
    // Береговой вал держит воду русла — но в озере её держит само
    // озеро, и вал посреди озера встал бы дамбой.
    if (!chan && levee > s && !(inLake && s <= lk)) s = levee;

    const bool lakeWater = inLake && s <= lk;
    if (!chan && !lakeWater && rim && s <= lk) s = lk + 1;

    if (chan) {
        const i32 top = lakeWater ? std::max(chanY, lk) : chanY;
        const i32 dep = std::max(1, (i32)std::lround(chanDepth));
        i32 bed = chanY - dep + 1;
        if (lakeWater) bed = std::min(bed, s);
        r.kind = ColumnWater::Channel;
        r.near = 1.f;
        r.surface = bed;
        r.waterTop = top;
        r.flow = lakeWater ? 0 : chanFlow;
        r.flags = chanFlags;
        r.frozen = (chanFlags & SF_FROZEN) != 0 || (lakeWater && lakeFrozen);
        return r;
    }
    if (lakeWater) {
        r.kind = ColumnWater::Lake;
        r.surface = s;
        r.waterTop = lk;
        r.frozen = lakeFrozen;
        return r;
    }
    if (s != raw || nearWater || bar) {
        r.kind = ColumnWater::Ground;
        r.surface = s;
        r.flags = groundFlags;
        r.bar = bar;
        r.bank = nearWater;
    }
    return r;
}

ColumnWater Hydrology::column(i32 wx, i32 wz, i32 raw) const {
    struct Cache {
        u32 owner = 0;
        i32 cx = 0, cz = 0;
        ChunkView view;
    };
    static thread_local Cache cache;
    const i32 cx = floorDiv(wx, CHUNK_SIZE), cz = floorDiv(wz, CHUNK_SIZE);
    if (cache.owner != id_ || cache.cx != cx || cache.cz != cz) {
        chunkView(cx, cz, cache.view);
        cache.owner = id_;
        cache.cx = cx;
        cache.cz = cz;
    }
    return evaluate(cache.view, wx, wz, raw);
}

} // namespace hydro
} // namespace world
