// ============================================================
// tools/worldmap — карта мира по слоям и цифры генератора.
//
// Глазами генератор проверяют с высоты птичьего полёта: там видно,
// есть ли у мира крупные формы или это ровный шум, как лежат хребты,
// куда текут реки и где кончается один биом и начинается другой. Из
// кадра с земли этого не понять — туман съедает всё дальше трёхсот
// блоков.
//
// Та же генерация, что в игре (TerrainGenerator, гидрология), без
// рендера: колонка за колонкой, пиксель за пикселем.
//
//   ./tools/worldmap/run.sh --seed 1 --layer relief --out build/worldmap/s1
//   ./tools/worldmap/run.sh --seed 1 --layer all --stats
//
// Слои: relief (рельеф с отмывкой, цвет поверхности, вода), height,
// biome, humidity, temperature, slope, water, landform, region.
// all — все подряд.
//
// --stats печатает цифры для сравнения до/после: распределение высот
// и уклонов, перепад высот по областям, доли биомов, резкость границ,
// доля воды, а также время генерации чанков (p50/p95/p99/худший).
// ============================================================
#include "world/terrain.h"
#include "world/features.h"
#include "world/block.h"
#include "world/chunk.h"
#include "render/iso_png.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace world;

namespace {

struct Opts {
    u64 seed = 1;
    i32 cx = 0, cz = 0;        ///< центр карты, блоки
    i32 px = 1024;             ///< сторона картинки, пиксели
    i32 scale = 4;             ///< блоков на пиксель
    std::string layer = "relief";
    std::string out = "build/worldmap/map";
    bool stats = false;
    i32 chunks = 400;          ///< сколько чанков мерить в --stats
};

struct Cell {
    TerrainGenerator::Column col;
    hydro::ColumnWater water;
    i32 ground = 0;            ///< земля после рек
};

u8 clamp8(f32 v) { return (u8)std::clamp(v, 0.f, 255.f); }

void rgbOf(u32 c, f32& r, f32& g, f32& b) {
    r = (f32)((c >> 24) & 0xFF); g = (f32)((c >> 16) & 0xFF); b = (f32)((c >> 8) & 0xFF);
}

/// Цвет для числа в [-1, 1]: синий — ноль — красный.
void diverging(f32 v, u8* o) {
    v = std::clamp(v, -1.f, 1.f);
    if (v < 0.f) { o[0] = clamp8(255 + v * 200); o[1] = clamp8(255 + v * 140); o[2] = 255; }
    else         { o[0] = 255; o[1] = clamp8(255 - v * 140); o[2] = clamp8(255 - v * 200); }
}

const u32 BIOME_RGB[BIOME_COUNT] = {
    0x2E5A9Cu, 0xE8D8A0u, 0x8CC66Au, 0x3E8C3Au, 0x4E7A5Au, 0xE0C878u,
    0xC0B060u, 0xE8F0F4u, 0x8A8A8Au, 0x5A7A4Au, 0x8A3A2Au, 0x4A3A3Au,
};

void grid(const Opts& o, const TerrainGenerator& gen, std::vector<Cell>& cells) {
    const i32 n = o.px;
    cells.resize((usize)n * n);
    const i32 x0 = o.cx - n * o.scale / 2, z0 = o.cz - n * o.scale / 2;
    for (i32 j = 0; j < n; ++j)
        for (i32 i = 0; i < n; ++i) {
            const i32 wx = x0 + i * o.scale, wz = z0 + j * o.scale;
            Cell& c = cells[(usize)j * n + i];
            c.col = gen.column(wx, wz);
            c.water = gen.hydrology().column(wx, wz, c.col.surface);
            c.ground = c.water.kind == hydro::ColumnWater::None ? c.col.surface
                                                                : c.water.surface;
        }
}

/// Вода в колонке: река, озеро или море.
bool wetCell(const Cell& c) {
    if (c.water.waterTop >= 0) return true;
    return c.ground <= TerrainGenerator::SEA_LEVEL;
}

void layerPixel(const Opts& o, const std::vector<Cell>& cells, i32 i, i32 j,
                const std::string& layer, u8* p)
{
    const i32 n = o.px;
    const Cell& c = cells[(usize)j * n + i];
    auto at = [&](i32 a, i32 b) -> const Cell& {
        a = std::clamp(a, 0, n - 1); b = std::clamp(b, 0, n - 1);
        return cells[(usize)b * n + a];
    };
    const f32 gx = (f32)(at(i + 1, j).ground - at(i - 1, j).ground) / (2.f * o.scale);
    const f32 gz = (f32)(at(i, j + 1).ground - at(i, j - 1).ground) / (2.f * o.scale);
    const f32 slope = std::sqrt(gx * gx + gz * gz);

    if (layer == "height") {
        const u8 v = clamp8((f32)c.ground * 2.f);
        p[0] = p[1] = p[2] = v;
    } else if (layer == "biome") {
        const u32 rgb = BIOME_RGB[c.col.climate.biome];
        p[0] = (u8)(rgb >> 16); p[1] = (u8)(rgb >> 8); p[2] = (u8)rgb;
    } else if (layer == "humidity") {
        diverging(-c.col.climate.humidity, p);
    } else if (layer == "temperature") {
        diverging(c.col.climate.temperature, p);
    } else if (layer == "slope") {
        const u8 v = clamp8(255.f - slope * 100.f);
        p[0] = p[1] = p[2] = v;
    } else if (layer == "water") {
        if (wetCell(c)) { p[0] = 40; p[1] = 90; p[2] = 200; }
        else if (c.water.bank) { p[0] = 120; p[1] = 170; p[2] = 230; }
        else { const u8 v = clamp8(40 + c.ground * 1.5f); p[0] = p[1] = p[2] = v; }
    } else if (layer == "landform") {
        const u32 rgb = landformDebugColor(c.col.landform);
        p[0] = (u8)(rgb >> 16); p[1] = (u8)(rgb >> 8); p[2] = (u8)rgb;
    } else if (layer == "region") {
        diverging(c.col.climate.region * 2.f - 1.f, p);
    } else {  // relief
        const BiomeDef& bd = BiomeField(0).def(c.col.climate.biome);
        u32 top = blocks().get(bd.surfaceBlock).colorTop;
        if (c.col.climate.biome == Mountains && c.ground > TerrainGenerator::SNOW_LINE)
            top = blocks().get(SNOW).colorTop;
        f32 r, g, b; rgbOf(top, r, g, b);
        // Отмывка: свет с северо-запада, как на бумажных картах.
        const f32 shade = std::clamp(1.f + (-gx - gz) * 0.55f, 0.45f, 1.45f);
        const f32 alt = 0.85f + (f32)c.ground / 300.f;
        r *= shade * alt; g *= shade * alt; b *= shade * alt;
        if (wetCell(c)) {
            const f32 depth = (f32)std::max(0, (c.water.waterTop >= 0 ? c.water.waterTop
                                                                      : TerrainGenerator::SEA_LEVEL)
                                               - c.ground);
            const f32 k = std::clamp(depth / 14.f, 0.f, 1.f);
            r = 70 - 40 * k; g = 130 - 50 * k; b = 210 - 40 * k;
        }
        p[0] = clamp8(r); p[1] = clamp8(g); p[2] = clamp8(b);
    }
}

void writeLayer(const Opts& o, const std::vector<Cell>& cells, const std::string& layer) {
    std::vector<u8> rgb((usize)o.px * o.px * 3);
    for (i32 j = 0; j < o.px; ++j)
        for (i32 i = 0; i < o.px; ++i)
            layerPixel(o, cells, i, j, layer, &rgb[((usize)j * o.px + i) * 3]);
    const std::string path = o.out + "_" + layer + ".png";
    if (render::writePngFile(path.c_str(), rgb.data(), (u32)o.px, (u32)o.px))
        std::printf("worldmap: %s\n", path.c_str());
    else
        std::printf("worldmap: не записан %s\n", path.c_str());
}

f64 pct(std::vector<f64>& v, f64 q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[(usize)std::min<f64>((f64)v.size() - 1, q * (f64)(v.size() - 1))];
}

void printStats(const Opts& o, const TerrainGenerator& gen, const std::vector<Cell>& cells) {
    const i32 n = o.px;
    std::vector<f64> hs, slopes;
    hs.reserve(cells.size()); slopes.reserve(cells.size());
    u32 biomeCount[BIOME_COUNT] = {};
    u32 edges = 0, pairs = 0, wet = 0, river = 0, steep2 = 0, steep4 = 0;
    for (i32 j = 0; j < n; ++j)
        for (i32 i = 0; i < n; ++i) {
            const Cell& c = cells[(usize)j * n + i];
            hs.push_back(c.ground);
            ++biomeCount[c.col.climate.biome];
            if (wetCell(c)) ++wet;
            if (c.water.kind == hydro::ColumnWater::Channel) ++river;
            if (i + 1 < n) {
                const Cell& r = cells[(usize)j * n + i + 1];
                ++pairs;
                if (r.col.climate.biome != c.col.climate.biome) ++edges;
                const f64 s = std::fabs((f64)(r.ground - c.ground)) / o.scale;
                slopes.push_back(s);
                if (s >= 2.0) ++steep2;
                if (s >= 4.0) ++steep4;
            }
        }
    const f64 N = (f64)cells.size();
    std::printf("\n== seed %llu, область %d блоков, шаг %d ==\n",
                (unsigned long long)o.seed, n * o.scale, o.scale);
    std::printf("высота земли      p5 %.0f  p50 %.0f  p95 %.0f  max %.0f\n",
                pct(hs, 0.05), pct(hs, 0.5), pct(hs, 0.95), pct(hs, 1.0));
    std::printf("уклон, блок/блок  p50 %.2f  p90 %.2f  p99 %.2f;  >=2: %.1f%%  >=4: %.2f%%\n",
                pct(slopes, 0.5), pct(slopes, 0.9), pct(slopes, 0.99),
                100.0 * steep2 / std::max<u32>(1, pairs), 100.0 * steep4 / std::max<u32>(1, pairs));

    // Перепад высот в окнах 256 блоков: насколько разный характер у
    // областей. Ровный шум даёт узкое распределение — все окна похожи.
    {
        const i32 w = std::max(1, 256 / o.scale);
        std::vector<f64> reliefs;
        for (i32 j0 = 0; j0 + w <= n; j0 += w)
            for (i32 i0 = 0; i0 + w <= n; i0 += w) {
                i32 lo = 1 << 30, hi = -(1 << 30);
                for (i32 j = j0; j < j0 + w; ++j)
                    for (i32 i = i0; i < i0 + w; ++i) {
                        const i32 g = cells[(usize)j * n + i].ground;
                        lo = std::min(lo, g); hi = std::max(hi, g);
                    }
                reliefs.push_back(hi - lo);
            }
        std::printf("перепад в окне 256  p10 %.0f  p50 %.0f  p90 %.0f  max %.0f  (окон %zu)\n",
                    pct(reliefs, 0.1), pct(reliefs, 0.5), pct(reliefs, 0.9),
                    pct(reliefs, 1.0), reliefs.size());
    }
    std::printf("вода %.1f%%, из неё русла %.2f%% площади\n", 100.0 * wet / N, 100.0 * river / N);
    std::printf("граница биомов: %.2f%% соседних пар (на шаг %d)\n",
                100.0 * edges / std::max<u32>(1, pairs), o.scale);
    std::printf("биомы:");
    for (u32 b = 0; b < BIOME_COUNT; ++b)
        if (biomeCount[b]) std::printf(" %s %.1f%%", gen.field().def((BiomeId)b).name,
                                       100.0 * biomeCount[b] / N);
    std::printf("\n");

    // ---- Генерация чанков ----
    if (o.chunks <= 0) return;
    auto chunk = std::make_unique<Chunk>();
    std::vector<TerrainGenerator::Column> cols;
    std::vector<f64> msCols, msGen;
    u64 state = o.seed * 0x9E3779B97F4A7C15ull + 7;
    auto rnd = [&]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    const i32 half = n * o.scale / 2 / CHUNK_SIZE;
    // Прогрев: плитки гидросети считаются один раз на 2000 блоков, и в
    // стоимость первого чанка они войти не должны.
    for (i32 k = 0; k < o.chunks; ++k) {
        const i32 ccx = o.cx / CHUNK_SIZE + (i32)(rnd() % (u64)(2 * half + 1)) - half;
        const i32 ccz = o.cz / CHUNK_SIZE + (i32)(rnd() % (u64)(2 * half + 1)) - half;
        gen.hydrology().tile(hydro::tileOfBlock(ccx * CHUNK_SIZE), hydro::tileOfBlock(ccz * CHUNK_SIZE));
    }
    state = o.seed * 0x9E3779B97F4A7C15ull + 7;
    for (i32 k = 0; k < o.chunks; ++k) {
        const i32 ccx = o.cx / CHUNK_SIZE + (i32)(rnd() % (u64)(2 * half + 1)) - half;
        const i32 ccz = o.cz / CHUNK_SIZE + (i32)(rnd() % (u64)(2 * half + 1)) - half;
        chunk->coord = { ccx, 0, ccz };
        const auto t0 = std::chrono::steady_clock::now();
        computeChunkColumns(gen, ccx, ccz, cols);
        const auto t1 = std::chrono::steady_clock::now();
        generateChunkVoxels(*chunk, gen, cols.data(), o.seed);
        const auto t2 = std::chrono::steady_clock::now();
        msCols.push_back(std::chrono::duration<f64, std::milli>(t1 - t0).count());
        msGen.push_back(std::chrono::duration<f64, std::milli>(t2 - t0).count());
    }
    std::printf("чанк: колонки p50 %.2f мс; целиком p50 %.2f  p95 %.2f  p99 %.2f  худший %.2f мс (%d чанков)\n",
                pct(msCols, 0.5), pct(msGen, 0.5), pct(msGen, 0.95), pct(msGen, 0.99),
                pct(msGen, 1.0), o.chunks);
}

} // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if      (a == "--seed")   o.seed = (u64)std::strtoull(next(), nullptr, 10);
        else if (a == "--center") { o.cx = std::atoi(next()); o.cz = std::atoi(next()); }
        else if (a == "--px")     o.px = std::max(16, std::atoi(next()));
        else if (a == "--scale")  o.scale = std::max(1, std::atoi(next()));
        else if (a == "--layer")  o.layer = next();
        else if (a == "--out")    o.out = next();
        else if (a == "--stats")  o.stats = true;
        else if (a == "--chunks") o.chunks = std::atoi(next());
        else { std::printf("worldmap: неизвестный ключ %s\n", a.c_str()); return 2; }
    }
    blocks();
    TerrainGenerator gen(o.seed);
    std::vector<Cell> cells;
    const auto t0 = std::chrono::steady_clock::now();
    grid(o, gen, cells);
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("worldmap: %d x %d колонок за %.0f мс (с гидросетью)\n", o.px, o.px, ms);

    static const char* ALL[] = { "relief", "height", "biome", "humidity", "temperature",
                                 "slope", "water", "landform", "region" };
    if (o.layer == "all") for (const char* l : ALL) writeLayer(o, cells, l);
    else if (o.layer != "none") writeLayer(o, cells, o.layer);
    if (o.stats) printStats(o, gen, cells);
    return 0;
}
