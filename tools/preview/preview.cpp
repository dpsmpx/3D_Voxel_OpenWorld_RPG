// ============================================================
// tools/preview — рендер мира в картинку, без устройства и без
// Vulkan.
//
// Зачем. Половина ошибок рендера видна только глазом: уровень
// детализации, рассыпающийся на плиты в воздухе; грани, вывернутые
// наизнанку; дыры в рельефе; затенение, съедающее боковые стороны в
// чёрное. Ни компилятор, ни тесты логики такого не ловят, а сборка
// APK и запуск на телефоне — длинный круг.
//
// Здесь берутся ТЕ ЖЕ генерация мира, ТОТ ЖЕ мешер и ТОТ ЖЕ
// построитель вершин, что в игре, и растеризуются простым z-буфером с
// теми же матрицами камеры. Освещение — приближение voxel.frag: цифры
// повторены вручную, так что тонкие различия в цвете тут не судить.
// Геометрия же совпадает точно, а ради неё всё и затевалось.
//
//   ./tools/preview/run.sh                  вид от игрока
//   ./tools/preview/run.sh --lod 1          тот же вид, огрублённый
//   ./tools/preview/run.sh --debug sky      отладочные каналы
// ============================================================
#include "world/chunk.h"
#include "world/block.h"
#include "world/terrain.h"
#include "world/features.h"
#include "render/mesh_builder.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace world;

namespace {

int   W = 900, H = 560;
int   g_lod = 0;
int   g_debug = 0;           // 1 небо, 2 затенение, 3 нормали, 4 материал
float g_yaw = 0.6f, g_pitch = -0.25f, g_height = 1.7f;
float g_px = 0.5f, g_pz = 0.5f;   ///< где стоит камера по горизонтали
int   g_radius = 3;          // чанков в каждую сторону
const char* g_out = "preview.ppm";

std::vector<float>     zbuf;
std::vector<glm::vec3> color;

glm::vec3 sunDir, skyColor, camPos;
float     skyLight = 1.f, fogStart = 0.f, fogEnd = 0.f;

const glm::vec3 FACE_N[6] = {
    { 1,0,0},{-1,0,0},{0, 1,0},{0,-1,0},{0,0, 1},{0,0,-1}
};

glm::vec3 toLin(glm::vec3 c) { return c * c; }
glm::vec3 toSrgb(glm::vec3 c) {
    return { std::sqrt(std::max(c.x,0.f)), std::sqrt(std::max(c.y,0.f)),
             std::sqrt(std::max(c.z,0.f)) };
}

float hash13(glm::vec3 p) {
    p = glm::fract(p * 0.1031f);
    p += glm::dot(p, glm::vec3(p.y, p.z, p.x) + 33.33f);
    const float v = std::fmod((p.x + p.y) * p.z, 1.0f);
    return v < 0.f ? v + 1.f : v;
}

struct Vtx {
    glm::vec3 world;
    glm::vec4 clip;
    float ao, sky, grain;
    int face, tint;
    glm::vec3 col;
};

glm::vec3 shade(const Vtx& v, const glm::vec3& wp, float ao01, float sky01) {
    if (g_debug == 1) return glm::vec3(sky01);
    if (g_debug == 2) return glm::vec3(ao01);
    if (g_debug == 3) return FACE_N[v.face] * 0.5f + 0.5f;
    if (g_debug == 4) return v.col;

    const glm::vec3 N = FACE_N[v.face];
    glm::vec3 albedo = toLin(v.col) * (1.f + v.grain * (hash13(glm::floor(wp - N*0.5f)) - 0.5f));
    if (v.tint) {
        const float t = std::sin(wp.x*0.021f + std::sin(wp.z*0.013f)*2.3f)*0.5f + 0.5f;
        albedo *= glm::mix(glm::vec3(0.82f,1.05f,0.78f), glm::vec3(1.14f,0.95f,0.66f), t);
    }
    const float hf = glm::clamp((wp.y - 40.f) / 70.f, 0.f, 1.f);
    albedo *= glm::mix(glm::vec3(0.90f,0.93f,1.00f), glm::vec3(1.05f,1.03f,0.97f), hf);

    const glm::vec3 skyLin = toLin(skyColor);
    const float day   = glm::clamp(skyLight, 0.f, 1.f);
    const float above = glm::smoothstep(-0.10f, 0.06f, sunDir.y);
    const glm::vec3 sunTint = toLin(glm::mix(glm::vec3(1.f,0.52f,0.26f),
                                             glm::vec3(1.f,0.97f,0.92f),
                                             glm::smoothstep(0.f,0.30f,sunDir.y)));
    const float skyMax = std::max(std::max(skyLin.x,skyLin.y), std::max(skyLin.z,0.001f));
    const glm::vec3 skyTint = glm::mix(glm::vec3(1.f), skyLin/skyMax, 0.60f);
    const glm::vec3 groundTint(0.62f, 0.56f, 0.46f);

    const float ao    = 0.42f + 0.58f*ao01;
    const float aoSun = glm::mix(1.f, ao, 0.6f);
    const float sky   = 0.18f + 0.82f*sky01;
    const float skySun = glm::smoothstep(0.35f, 0.85f, sky01);
    const float skyVis = 0.5f + 0.5f*N.y;

    const glm::vec3 hemi = glm::mix(groundTint, skyTint, skyVis);
    const float amb = glm::mix(0.34f, 0.66f, skyVis) * (0.30f + 0.70f*day);
    const float ndl = std::max(glm::dot(N, sunDir), 0.f);

    glm::vec3 lit = albedo * (hemi*amb*ao*sky + sunTint*(ndl*0.52f*day*above)*aoSun*skySun + 0.02f);

    const glm::vec3 toFrag = wp - camPos;
    const float dist = glm::length(toFrag);
    float fogAmt = glm::clamp((dist-fogStart)/std::max(fogEnd-fogStart, 0.001f), 0.f, 1.f);
    fogAmt = fogAmt*fogAmt*(3.f-2.f*fogAmt);
    const float sunAmt = std::max(glm::dot(glm::normalize(toFrag), sunDir), 0.f);
    const glm::vec3 fogColor = glm::mix(skyLin, sunTint, std::pow(sunAmt,8.f)*0.30f*above);
    return toSrgb(glm::mix(lit, fogColor, fogAmt));
}

/// Линейная смесь двух вершин. Плоские атрибуты (грань, зерно,
/// подкраска, цвет) берём у первой: внутри квада они одинаковы.
Vtx lerpVtx(const Vtx& p, const Vtx& q, float t) {
    Vtx r = p;
    r.clip  = p.clip  + (q.clip  - p.clip)  * t;
    r.world = p.world + (q.world - p.world) * t;
    r.ao    = p.ao    + (q.ao    - p.ao)    * t;
    r.sky   = p.sky   + (q.sky   - p.sky)   * t;
    return r;
}

void rasterize(const Vtx& a, const Vtx& b, const Vtx& c);

/// Отсечение по ближней плоскости.
///
/// Раньше треугольник, у которого хоть одна вершина оказалась позади
/// камеры, выбрасывался целиком. На полном разрешении это почти
/// незаметно: под ногами пропадала пара мелких граней. Но жадное
/// слияние на огрублённых уровнях собирает пол-чанка в один квад — и
/// достаточно одному его углу уйти за спину, чтобы исчез весь. Земля
/// под камерой пропадала пластами, а в небе оставались обрывки
/// соседних квадов: ровно та картинка, которую ищут как ошибку
/// мешера. Инструмент врал именно там, где он нужнее всего.
void triangle(const Vtx& a, const Vtx& b, const Vtx& c) {
    constexpr float EPS = 1e-4f;
    const Vtx* in[3] = { &a, &b, &c };

    Vtx poly[4];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const Vtx& cur = *in[i];
        const Vtx& nxt = *in[(i + 1) % 3];
        const bool curIn = cur.clip.w > EPS;
        const bool nxtIn = nxt.clip.w > EPS;
        if (curIn) poly[n++] = cur;
        if (curIn != nxtIn) {
            const float t = (EPS - cur.clip.w) / (nxt.clip.w - cur.clip.w);
            poly[n++] = lerpVtx(cur, nxt, t);
        }
    }
    if (n < 3) return;
    rasterize(poly[0], poly[1], poly[2]);
    if (n == 4) rasterize(poly[0], poly[2], poly[3]);
}

void rasterize(const Vtx& a, const Vtx& b, const Vtx& c) {
    const glm::vec3 A = glm::vec3(a.clip)/a.clip.w;
    const glm::vec3 B = glm::vec3(b.clip)/b.clip.w;
    const glm::vec3 C = glm::vec3(c.clip)/c.clip.w;
    auto sx = [](const glm::vec3& p){ return (p.x*0.5f+0.5f)*(float)W; };
    auto sy = [](const glm::vec3& p){ return (p.y*0.5f+0.5f)*(float)H; };
    const float ax=sx(A), ay=sy(A), bx=sx(B), by=sy(B), cx=sx(C), cy=sy(C);

    // Лицевые грани — по часовой в координатах кадра: ровно то же
    // правило, что у VK_FRONT_FACE_CLOCKWISE в конвейере.
    const float area = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
    if (area >= 0.f) return;

    const int minx = std::max(0,   (int)std::floor(std::min({ax,bx,cx})));
    const int maxx = std::min(W-1, (int)std::ceil (std::max({ax,bx,cx})));
    const int miny = std::max(0,   (int)std::floor(std::min({ay,by,cy})));
    const int maxy = std::min(H-1, (int)std::ceil (std::max({ay,by,cy})));
    const float den = (by-cy)*(ax-cx) + (cx-bx)*(ay-cy);
    if (std::fabs(den) < 1e-9f) return;

    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            const float px = x+0.5f, py = y+0.5f;
            const float l0 = ((by-cy)*(px-cx) + (cx-bx)*(py-cy)) / den;
            const float l1 = ((cy-ay)*(px-cx) + (ax-cx)*(py-cy)) / den;
            const float l2 = 1.f - l0 - l1;
            if (l0 < 0.f || l1 < 0.f || l2 < 0.f) continue;
            const float z = l0*A.z + l1*B.z + l2*C.z;
            if (z < 0.f || z > 1.f) continue;
            const int idx = y*W + x;
            if (z >= zbuf[idx]) continue;
            zbuf[idx] = z;
            color[idx] = shade(a,
                               a.world*l0 + b.world*l1 + c.world*l2,
                               a.ao*l0 + b.ao*l1 + c.ao*l2,
                               a.sky*l0 + b.sky*l1 + c.sky*l2);
        }
    }
}

std::shared_ptr<Chunk> makeChunk(const TerrainGenerator& gen, u64 seed, i32 cx, i32 cz) {
    auto c = std::make_shared<Chunk>();
    c->coord = { cx, 0, cz };
    // Ровно тот же путь, которым чанки строит игра: раскладка слоёв и
    // порядок фич живут в world::generateChunkVoxels. Раньше они были
    // выписаны и здесь тоже, и офлайн-рендер мог показывать не тот
    // мир, который на устройстве, — то есть ровно тогда врать, когда
    // он и нужен.
    static std::vector<TerrainGenerator::Column> cols;
    computeChunkColumns(gen, cx, cz, cols);
    generateChunkVoxels(*c, gen, cols.data(), seed);
    c->generated.store(true);
    return c;
}

} // namespace

int main(int argc, char** argv) {
    u64 seed = 1234;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i+1 < argc) ? argv[++i] : "0"; };
        if      (a == "--lod")    g_lod = atoi(next());
        else if (a == "--yaw")    g_yaw = (float)atof(next());
        else if (a == "--pitch")  g_pitch = (float)atof(next());
        else if (a == "--height") g_height = (float)atof(next());
        else if (a == "--radius") g_radius = atoi(next());
        else if (a == "--seed")   seed = (u64)strtoull(next(), nullptr, 10);
        else if (a == "--pos")    { g_px = (float)atof(next()); g_pz = (float)atof(next()); }
        else if (a == "--size")   { W = atoi(next()); H = atoi(next()); }
        else if (a == "--out")    g_out = next();
        else if (a == "--debug") {
            const std::string d = next();
            g_debug = d == "sky" ? 1 : d == "ao" ? 2 : d == "normal" ? 3 :
                      d == "material" ? 4 : 0;
        } else {
            std::printf("preview: неизвестный ключ %s\n", a.c_str());
            return 2;
        }
    }

    blocks();
    TerrainGenerator gen(seed);

    std::map<std::pair<i32,i32>, std::shared_ptr<Chunk>> map;
    for (i32 cz = -g_radius; cz <= g_radius; ++cz)
        for (i32 cx = -g_radius; cx <= g_radius; ++cx)
            map[{cx,cz}] = makeChunk(gen, seed, cx, cz);

    // Встать там же, где стоял игрок: журнал с устройства печатает
    // положение камеры, и сравнивать картинки надо с одной точки.
    camPos   = { g_px, (f32)gen.surfaceHeight((i32)g_px, (i32)g_pz) + g_height, g_pz };
    sunDir   = glm::normalize(glm::vec3(0.35f, 0.78f, 0.25f));
    skyColor = { 0.55f, 0.72f, 0.92f };
    const float vd = (float)(g_radius * CHUNK_SIZE);
    fogStart = vd*0.55f; fogEnd = vd*0.94f;

    auto P = glm::perspective(glm::radians(70.f), (float)W/(float)H, 0.1f, 512.f);
    P[1][1] *= -1.f;
    const glm::vec3 fwd{ std::cos(g_pitch)*std::sin(g_yaw), std::sin(g_pitch),
                         std::cos(g_pitch)*std::cos(g_yaw) };
    const glm::mat4 VP = P * glm::lookAt(camPos, camPos + fwd, glm::vec3(0,1,0));

    zbuf.assign((usize)W*H, 1e9f);
    color.assign((usize)W*H, skyColor);

    std::vector<Quad> quads;
    std::vector<render::VoxelVertex> verts;
    std::vector<u32> idx;
    u32 opaque = 0;
    usize totalQuads = 0;

    for (auto& [key, c] : map) {
        ChunkNeighbors nb{};
        auto at = [&](i32 x, i32 z) -> const Chunk* {
            auto it = map.find({x,z});
            return it == map.end() ? nullptr : it->second.get();
        };
        nb.nx = at(key.first-1, key.second); nb.px = at(key.first+1, key.second);
        nb.nz = at(key.first, key.second-1); nb.pz = at(key.first, key.second+1);

        buildGreedyMesh(*c, nb, quads, (Lod)g_lod);
        render::buildChunkVertices(*c, quads, verts, idx, opaque);
        totalQuads += quads.size();

        const glm::vec3 origin{ (float)key.first*CHUNK_SIZE, 0.f,
                                (float)key.second*CHUNK_SIZE };
        std::vector<Vtx> vv(verts.size());
        for (usize i = 0; i < verts.size(); ++i) {
            const u32 p = verts[i].packed;
            const glm::vec3 local{ (float)( p        & 63u),
                                   (float)((p >>  6) & 255u),
                                   (float)((p >> 14) & 63u) };
            vv[i].world = origin + local;
            vv[i].clip  = VP * glm::vec4(vv[i].world, 1.f);
            vv[i].face  = (int)((p >> 20) & 7u);
            vv[i].ao    = (float)((p >> 23) & 3u) / 3.f;
            vv[i].sky   = (float)((p >> 25) & 7u) / 7.f;
            vv[i].grain = (float)((p >> 28) & 7u) / 7.f * 0.30f;
            vv[i].tint  = (int)((p >> 31) & 1u);
            vv[i].col   = glm::vec3(verts[i].r, verts[i].g, verts[i].b) / 255.f;
        }
        // Рисуем только непрозрачную часть: смешивание тут ни к чему.
        for (u32 i = 0; i + 2 < opaque; i += 3)
            triangle(vv[idx[i]], vv[idx[i+1]], vv[idx[i+2]]);
    }

    std::FILE* f = std::fopen(g_out, "wb");
    if (!f) { std::printf("preview: не открыть %s\n", g_out); return 1; }
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W*H; ++i) {
        const unsigned char px[3] = {
            (unsigned char)glm::clamp(color[i].x*255.f, 0.f, 255.f),
            (unsigned char)glm::clamp(color[i].y*255.f, 0.f, 255.f),
            (unsigned char)glm::clamp(color[i].z*255.f, 0.f, 255.f) };
        std::fwrite(px, 1, 3, f);
    }
    std::fclose(f);
    std::printf("preview: чанков %zu, квадов %zu, уровень %d -> %s (%dx%d)\n",
                map.size(), totalQuads, g_lod, g_out, W, H);
    return 0;
}
