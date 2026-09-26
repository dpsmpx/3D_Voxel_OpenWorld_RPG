/**
 * @file flora_models.cpp
 * @brief Рендер: модели растений и мелких природных вещей из мелких вокселей.
 */
#include "flora_models.h"
#include "../world/block.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <vector>

namespace render {

namespace {

using world::FloraKind;

constexpr f32 TAU = 6.2831853f;

// ---- Цвет: 0xRRGGBBAA, как у блоков и VoxelModel ----

u32 rgb(u8 r, u8 g, u8 b) { return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFFu; }
u32 opaque(u32 c) { return c | 0xFFu; }

u32 shade(u32 c, f32 k) {
    auto ch = [&](u32 s) {
        const f32 v = (f32)((c >> s) & 0xFFu) * k;
        return (u32)std::clamp(v, 0.f, 255.f) << s;
    };
    return ch(24) | ch(16) | ch(8) | 0xFFu;
}

u32 hash3(i32 x, i32 y, i32 z, u32 seed) {
    u32 h = seed * 0x9E3779B1u;
    h ^= (u32)x * 374761393u;
    h ^= (u32)y * 668265263u;
    h ^= (u32)z * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/// Сетка ступени: размер вокселя и насколько раздуть объёмные формы.
///
/// На грубой ступени крона и лапы ели, пересчитанные честно, худеют:
/// воксель попадает внутрь, только если внутрь попала его середина,
/// и конус ели в три блока шириной становится крестом. Издалека такое
/// дерево читается палкой. Раздутие на треть вокселя возвращает массу.
struct Grid {
    f32 vs;
    f32 grow;
};

/// Пятнистость. Оттенок меняется пятнами в `patch` блока, а не по
/// вокселю: соседние грани одного цвета сливаются жадным мешером, и
/// крона не рассыпается на тысячи отдельных квадов. Три уровня.
///
/// На дальней ступени (воксель — блок и крупнее) пятен нет вовсе:
/// издалека их не разглядеть, а каждое пятно — отдельный квад, и лес
/// у горизонта стоил бы вдвое дороже ровно за то, чего не видно.
u32 mottle(const Grid& g, u32 c, const glm::vec3& p, u32 seed, f32 amp, f32 patch = 0.5f) {
    if (g.vs >= 1.f) return c;
    patch = std::max(patch, g.vs * 2.f);
    const u32 h = hash3((i32)std::floor(p.x / patch), (i32)std::floor(p.y / patch),
                        (i32)std::floor(p.z / patch), seed);
    return shade(c, 1.f + amp * (f32)((i32)(h % 3u) - 1));
}

/// Низ кроны темнее верха: свету туда дальше. Ступенями, по той же
/// причине, что и пятна.
f32 depthShade(f32 y, f32 y0, f32 y1) {
    const f32 t = std::clamp((y - y0) / std::max(y1 - y0, 0.01f), 0.f, 1.f);
    return 0.80f + 0.20f * std::round(t * 3.f) / 3.f;
}

/// Параметры формы из зерна (вид, вариант). Одна и та же
/// последовательность на всех ступенях: ступень меняет сетку, но не
/// форму.
struct Rng {
    u32 s;
    f32 next() { s = s * 1664525u + 1013904223u; return (f32)(s >> 8) * (1.f / 16777216.f); }
    f32 range(f32 a, f32 b) { return a + (b - a) * next(); }
};

/// Лепка модели формами в блоках. Начало — низ, середина; сетка
/// нечётной ширины, чтобы середина пришлась на воксель и ствол на
/// оси не распадался на два столбца.
struct Sculpt {
    VoxelModel m;
    f32 vs;
    f32 grow;
    f32 c0;   ///< середина нулевого вокселя по X и Z, в блоках

    Sculpt(f32 halfW, f32 height, Grid g) : vs(g.vs), grow(g.grow) {
        i32 n = std::max(1, (i32)std::ceil(2.f * halfW / vs));
        if (n % 2 == 0) ++n;
        const i32 ny = std::max(1, (i32)std::ceil(height / vs));
        m.resize(n, ny, n);
        m.voxelSize = vs;
        c0 = -(f32)(n - 1) * 0.5f * vs;
    }

    glm::vec3 centre(i32 x, i32 y, i32 z) const {
        return { c0 + (f32)x * vs, ((f32)y + 0.5f) * vs, c0 + (f32)z * vs };
    }

    template <class Inside, class Color>
    void fill(const glm::vec3& lo, const glm::vec3& hi, Inside&& inside, Color&& color) {
        const i32 xa = std::max(0, (i32)std::floor((lo.x - c0) / vs));
        const i32 xb = std::min(m.sx - 1, (i32)std::ceil((hi.x - c0) / vs));
        const i32 za = std::max(0, (i32)std::floor((lo.z - c0) / vs));
        const i32 zb = std::min(m.sz - 1, (i32)std::ceil((hi.z - c0) / vs));
        const i32 ya = std::max(0, (i32)std::floor(lo.y / vs - 0.5f));
        const i32 yb = std::min(m.sy - 1, (i32)std::ceil(hi.y / vs));
        for (i32 y = ya; y <= yb; ++y)
            for (i32 z = za; z <= zb; ++z)
                for (i32 x = xa; x <= xb; ++x) {
                    const glm::vec3 p = centre(x, y, z);
                    if (inside(p)) m.set(x, y, z, color(p));
                }
    }

    /// Эллипсоид. Не мельче вокселя: иначе мелкий комок кроны
    /// пропадает на грубой ступени.
    template <class Color>
    void ellipsoid(const glm::vec3& c, const glm::vec3& r, Color&& color) {
        const glm::vec3 re = glm::max(r + glm::vec3(grow), glm::vec3(vs * 0.5f));
        fill(c - re, c + re,
             [&](const glm::vec3& p) { const glm::vec3 d = (p - c) / re; return glm::dot(d, d) <= 1.f; },
             color);
    }

    /// Отрезок толщиной r. Тоньше вокселя не бывает: ветвь рассыпалась
    /// бы в точки.
    template <class Color>
    void segment(const glm::vec3& a, const glm::vec3& b, f32 r, Color&& color) {
        const f32 re = std::max(r, vs * 0.62f);
        const glm::vec3 ab = b - a;
        const f32 len2 = std::max(glm::dot(ab, ab), 1e-6f);
        fill(glm::min(a, b) - glm::vec3(re), glm::max(a, b) + glm::vec3(re),
             [&](const glm::vec3& p) {
                 const f32 t = std::clamp(glm::dot(p - a, ab) / len2, 0.f, 1.f);
                 const glm::vec3 d = p - (a + ab * t);
                 return glm::dot(d, d) <= re * re;
             },
             color);
    }

    /// Вертикальный цилиндр — ствол. На оси модели толщина не
    /// ограничена снизу: осевой столбец вокселей есть всегда.
    template <class Color>
    void cylinder(f32 x, f32 z, f32 y0, f32 y1, f32 r, Color&& color) {
        const f32 re = (x == 0.f && z == 0.f) ? r : std::max(r, vs * 0.5f);
        fill({ x - re, y0, z - re }, { x + re, y1, z + re },
             [&](const glm::vec3& p) {
                 const f32 dx = p.x - x, dz = p.z - z;
                 return p.y >= y0 && p.y <= y1 && dx * dx + dz * dz <= re * re;
             },
             color);
    }

    /// Конус — ярус хвои: основание радиусом r на высоте base.y.
    template <class Color>
    void cone(const glm::vec3& base, f32 r, f32 h, Color&& color) {
        const f32 R = r + grow;
        fill(base - glm::vec3(R, 0.f, R), base + glm::vec3(R, h, R),
             [&](const glm::vec3& p) {
                 if (p.y < base.y || p.y > base.y + h) return false;
                 const f32 rad = r * (1.f - (p.y - base.y) / h) + grow;
                 const f32 dx = p.x - base.x, dz = p.z - base.z;
                 return dx * dx + dz * dz <= rad * rad;
             },
             color);
    }
};

// ---- Палитра: от блоков, чтобы растения и земля были из одной игры ----

struct Palette {
    u32 leaf, bark, cut, grass, dry, stone, cactusTop, cactusSide;
};

const Palette& pal() {
    static const Palette p = [] {
        const auto& r = world::blocks();
        Palette q;
        q.leaf       = opaque(r.get(world::LEAVES).colorTop);
        q.bark       = opaque(r.get(world::WOOD).colorSide);
        q.cut        = opaque(r.get(world::WOOD).colorTop);
        q.grass      = opaque(r.get(world::GRASS).colorTop);
        q.dry        = opaque(r.get(world::DRY_GRASS).colorTop);
        q.stone      = opaque(r.get(world::STONE).colorSide);
        q.cactusTop  = opaque(r.get(world::CACTUS).colorTop);
        q.cactusSide = opaque(r.get(world::CACTUS).colorSide);
        return q;
    }();
    return p;
}

// ============================================================
// Деревья: воксель в четверть блока
// ============================================================

VoxelModel oak(Rng& r, u8 v, Grid vs) {
    const Palette& P = pal();
    Sculpt s(2.4f, 5.0f, vs);
    const f32 top = 2.5f + r.range(-0.2f, 0.3f);
    s.cylinder(0.f, 0.f, 0.f, top + 0.6f, 0.36f,
               [&](const glm::vec3& p) { return mottle(vs, P.bark, p, 11u, 0.08f); });
    for (i32 i = 0; i < 2; ++i) {
        const f32 a = r.range(0.f, TAU);
        s.segment({ 0.f, top - 0.5f + r.range(0.f, 0.4f), 0.f },
                  { std::cos(a) * 0.9f, top + 0.5f, std::sin(a) * 0.9f }, 0.13f,
                  [&](const glm::vec3&) { return P.bark; });
    }
    const f32 cy = top + 0.9f;
    auto leaf = [&](const glm::vec3& p) {
        return mottle(vs, shade(P.leaf, depthShade(p.y, top, top + 2.2f)), p, 23u, 0.07f);
    };
    s.ellipsoid({ 0.f, cy, 0.f }, { 1.35f, 1.0f, 1.35f }, leaf);
    const i32 n = 4 + (v & 1);
    for (i32 i = 0; i < n; ++i) {
        const f32 a = (f32)i * TAU / (f32)n + r.range(-0.4f, 0.4f);
        const f32 d = r.range(0.8f, 1.15f);
        const f32 y = cy + r.range(-0.45f, 0.45f);
        const f32 rr = r.range(0.7f, 0.95f);
        s.ellipsoid({ std::cos(a) * d, y, std::sin(a) * d }, { rr, rr * 0.75f, rr }, leaf);
    }
    return std::move(s.m);
}

VoxelModel birch(Rng& r, u8, Grid vs) {
    Sculpt s(1.6f, 6.0f, vs);
    const f32 H = 4.2f + r.range(-0.3f, 0.4f);
    const u32 bark = rgb(226, 222, 210), mark = rgb(58, 54, 50);
    s.cylinder(0.f, 0.f, 0.f, H, 0.26f, [&](const glm::vec3& p) {
        const u32 h = hash3((i32)std::floor(p.y / 0.25f),
                            (i32)std::floor(std::atan2(p.z, p.x) * 1.3f), 0, 77u);
        return h % 5u == 0u ? mark : bark;
    });
    const u32 leafC = rgb(132, 172, 72);
    auto leaf = [&](const glm::vec3& p) {
        return mottle(vs, shade(leafC, depthShade(p.y, H - 2.2f, H + 1.2f)), p, 29u, 0.07f);
    };
    s.ellipsoid({ 0.f, H - 0.6f, 0.f }, { 0.95f, 1.3f, 0.95f }, leaf);
    s.ellipsoid({ r.range(-0.35f, 0.35f), H + 0.35f, r.range(-0.35f, 0.35f) }, { 0.7f, 0.9f, 0.7f }, leaf);
    s.ellipsoid({ r.range(-0.4f, 0.4f), H - 1.5f, r.range(-0.4f, 0.4f) }, { 0.85f, 0.7f, 0.85f }, leaf);
    return std::move(s.m);
}

VoxelModel pine(Rng& r, u8 v, Grid vs) {
    Sculpt s(1.9f, 7.6f, vs);
    const f32 H = 6.4f + r.range(-0.4f, 0.5f);
    const u32 bark = rgb(96, 70, 48), needle = rgb(48, 96, 62);
    s.cylinder(0.f, 0.f, 0.f, H * 0.8f, 0.26f, [&](const glm::vec3&) { return bark; });
    const i32 n = 5 + (v & 1);
    const f32 y0 = 1.0f + r.range(0.f, 0.3f);
    for (i32 i = 0; i < n; ++i) {
        const f32 t = (f32)i / (f32)n;
        const f32 y = y0 + (H - y0 - 1.0f) * t;
        const f32 R = 1.75f * (1.f - t * 0.8f) + r.range(-0.1f, 0.1f);
        const f32 h = 1.3f * (1.f - t * 0.4f);
        const f32 tone = 0.84f + 0.16f * t;   // нижние лапы в тени верхних
        s.cone({ 0.f, y, 0.f }, R, h, [&](const glm::vec3& p) {
            return mottle(vs, shade(needle, tone), p, 31u, 0.06f);
        });
    }
    s.cone({ 0.f, H - 1.0f, 0.f }, 0.45f, 1.3f, [&](const glm::vec3&) { return needle; });
    return std::move(s.m);
}

VoxelModel acacia(Rng& r, u8, Grid vs) {
    const Palette& P = pal();
    Sculpt s(2.6f, 4.2f, vs);
    const f32 a = r.range(0.f, TAU);
    const glm::vec3 fork{ 0.f, 1.2f, 0.f };
    const glm::vec3 e1{ std::cos(a) * 1.1f, 2.7f, std::sin(a) * 1.1f };
    const glm::vec3 e2{ -std::cos(a) * 0.9f, 2.6f, -std::sin(a) * 0.9f };
    auto bark = [&](const glm::vec3& p) { return mottle(vs, P.bark, p, 13u, 0.07f); };
    s.cylinder(0.f, 0.f, 0.f, fork.y, 0.26f, bark);
    s.segment(fork, e1, 0.18f, bark);
    s.segment(fork, e2, 0.16f, bark);
    const u32 leafC = rgb(122, 142, 60);
    auto leaf = [&](const glm::vec3& p) { return mottle(vs, shade(leafC, depthShade(p.y, 2.6f, 3.4f)), p, 37u, 0.06f); };
    s.ellipsoid({ e1.x, 2.95f, e1.z }, { 1.35f, 0.32f, 1.2f }, leaf);
    s.ellipsoid({ e2.x, 2.85f, e2.z }, { 1.1f, 0.28f, 1.0f }, leaf);
    s.ellipsoid({ 0.f, 3.2f, 0.f }, { 0.9f, 0.25f, 0.9f }, leaf);
    return std::move(s.m);
}

VoxelModel palm(Rng& r, u8, Grid vs) {
    Sculpt s(2.8f, 6.4f, vs);
    const f32 H = 4.8f + r.range(-0.3f, 0.4f);
    const f32 a = r.range(0.f, TAU), lean = 0.45f;
    auto at = [&](f32 t) {
        return glm::vec3(std::cos(a) * lean * t * t, H * t, std::sin(a) * lean * t * t);
    };
    const u32 barkC = rgb(150, 118, 80);
    auto bark = [&](const glm::vec3& p) {
        return ((i32)std::floor(p.y / 0.3f) & 1) ? shade(barkC, 0.86f) : barkC;
    };
    constexpr i32 SEG = 6;
    for (i32 i = 0; i < SEG; ++i) {
        const f32 t0 = (f32)i / SEG, t1 = (f32)(i + 1) / SEG;
        s.segment(at(t0), at(t1), 0.24f - 0.06f * t1, bark);
    }
    const glm::vec3 top = at(1.f);
    const u32 frond = rgb(84, 156, 64);
    const i32 n = 7;
    for (i32 i = 0; i < n; ++i) {
        const f32 b = (f32)i * TAU / (f32)n + r.range(-0.2f, 0.2f);
        const f32 L = r.range(1.7f, 2.1f);
        glm::vec3 prev = top;
        for (i32 k = 1; k <= 5; ++k) {
            const f32 q = (f32)k / 5.f;
            const glm::vec3 cur = top + glm::vec3(std::cos(b) * L * q, 0.35f * q - 1.25f * q * q,
                                                  std::sin(b) * L * q);
            s.segment(prev, cur, 0.16f * (1.f - q * 0.5f),
                      [&](const glm::vec3& p) { return mottle(vs, frond, p, 41u, 0.07f); });
            prev = cur;
        }
    }
    const u32 nut = rgb(110, 78, 44);
    for (i32 i = 0; i < 3; ++i) {
        const f32 b = r.range(0.f, TAU);
        s.ellipsoid(top + glm::vec3(std::cos(b) * 0.2f, -0.25f, std::sin(b) * 0.2f),
                    glm::vec3(0.13f), [&](const glm::vec3&) { return nut; });
    }
    return std::move(s.m);
}

VoxelModel deadTree(Rng& r, u8 v, Grid vs) {
    Sculpt s(1.9f, 5.6f, vs);
    const f32 H = 3.6f + r.range(-0.3f, 0.5f);
    // Второй вариант — сухостой Чёрного леса: темнее.
    const u32 wood = v == 1 ? rgb(62, 54, 50) : rgb(98, 86, 74);
    auto bark = [&](const glm::vec3& p) { return mottle(vs, wood, p, 43u, 0.07f); };
    s.cylinder(0.f, 0.f, 0.f, H, 0.28f, bark);
    s.segment({ 0.f, H, 0.f }, { r.range(-0.3f, 0.3f), H + 0.6f, r.range(-0.3f, 0.3f) }, 0.14f, bark);
    const i32 n = 4 + (v & 1);
    for (i32 i = 0; i < n; ++i) {
        const f32 y = r.range(1.4f, H - 0.3f);
        const f32 a = (f32)i * TAU / (f32)n + r.range(-0.5f, 0.5f);
        const f32 len = r.range(0.8f, 1.4f);
        const glm::vec3 from{ 0.f, y, 0.f };
        const glm::vec3 to{ std::cos(a) * len, y + len * r.range(0.4f, 0.9f), std::sin(a) * len };
        s.segment(from, to, 0.12f, bark);
        const glm::vec3 mid = from + (to - from) * 0.6f;
        const f32 side = r.next() < 0.5f ? 1.f : -1.f;
        s.segment(mid, mid + glm::vec3(-std::sin(a) * 0.3f * side, 0.4f, std::cos(a) * 0.3f * side),
                  0.08f, bark);
    }
    return std::move(s.m);
}

VoxelModel cactus(Rng& r, u8 v, Grid vs) {
    const Palette& P = pal();
    Sculpt s(0.9f, 2.8f, vs);
    const f32 H = 2.0f + r.range(-0.2f, 0.4f);
    auto skin = [&](const glm::vec3& p) {
        // Рёбра: полосы по углу; маковка светлее.
        const i32 rib = (i32)std::floor((std::atan2(p.z, p.x) + 3.1415927f) * 8.f / TAU);
        const u32 c = p.y > H - 0.2f ? P.cactusTop : P.cactusSide;
        return (rib & 1) ? shade(c, 0.86f) : c;
    };
    s.cylinder(0.f, 0.f, 0.f, H, 0.3f, skin);
    const i32 arms = v == 0 ? 1 : (v == 1 ? 2 : 0);
    const f32 a0 = r.range(0.f, TAU);
    for (i32 i = 0; i < arms; ++i) {
        const f32 a = a0 + (f32)i * 3.1415927f;
        const f32 y = r.range(0.7f, 1.1f), up = r.range(0.5f, 0.8f);
        const glm::vec3 elbow{ std::cos(a) * 0.55f, y, std::sin(a) * 0.55f };
        s.segment({ 0.f, y, 0.f }, elbow, 0.2f, skin);
        s.segment(elbow, elbow + glm::vec3(0.f, up, 0.f), 0.2f, skin);
    }
    if (v == 2)   // молодой кактус цветёт
        s.ellipsoid({ 0.f, H + 0.05f, 0.f }, { 0.14f, 0.08f, 0.14f },
                    [&](const glm::vec3&) { return rgb(232, 120, 160); });
    return std::move(s.m);
}

// ============================================================
// Подлесок и мелочь: воксель в восьмую блока
// ============================================================

VoxelModel bush(Rng& r, u8 v, Grid vs) {
    Sculpt s(0.85f, 1.1f, vs);
    const u32 leafC = rgb(72, 128, 56), berry = rgb(196, 48, 48);
    auto leaf = [&](const glm::vec3& p) {
        if (v == 2) {
            const u32 h = hash3((i32)std::floor(p.x / 0.125f), (i32)std::floor(p.y / 0.125f),
                                (i32)std::floor(p.z / 0.125f), 53u);
            if (h % 23u == 0u) return berry;
        }
        return mottle(vs, shade(leafC, depthShade(p.y, 0.1f, 0.9f)), p, 47u, 0.07f, 0.25f);
    };
    const i32 n = 3 + (v & 1);
    for (i32 i = 0; i < n; ++i) {
        const f32 a = (f32)i * TAU / (f32)n + r.range(-0.5f, 0.5f);
        const f32 d = r.range(0.15f, 0.35f);
        const f32 rad = r.range(0.32f, 0.45f);
        s.ellipsoid({ std::cos(a) * d, r.range(0.35f, 0.55f), std::sin(a) * d },
                    { rad, rad * 0.85f, rad }, leaf);
    }
    return std::move(s.m);
}

VoxelModel dryBush(Rng& r, u8 v, Grid vs) {
    Sculpt s(0.7f, 0.95f, vs);
    const u32 twig = rgb(132, 104, 70), leafC = rgb(150, 140, 80);
    const i32 n = 7 + v * 2;
    for (i32 i = 0; i < n; ++i) {
        const f32 a = r.range(0.f, TAU);
        const f32 len = r.range(0.4f, 0.65f);
        const glm::vec3 end{ std::cos(a) * len * 0.9f, r.range(0.35f, 0.8f), std::sin(a) * len * 0.9f };
        s.segment({ 0.f, 0.f, 0.f }, end, 0.04f, [&](const glm::vec3&) { return twig; });
        const glm::vec3 mid = end * 0.6f;
        s.segment(mid, mid + glm::vec3(-std::sin(a) * 0.15f, 0.12f, std::cos(a) * 0.15f), 0.03f,
                  [&](const glm::vec3&) { return twig; });
        if (r.next() < 0.5f)
            s.ellipsoid(end, glm::vec3(0.06f), [&](const glm::vec3&) { return leafC; });
    }
    return std::move(s.m);
}

VoxelModel fern(Rng& r, u8 v, Grid vs) {
    Sculpt s(0.9f, 0.7f, vs);
    const u32 green = rgb(74, 134, 62);
    auto col = [&](const glm::vec3& p) { return mottle(vs, green, p, 59u, 0.08f, 0.25f); };
    const i32 n = 6 + v;
    for (i32 i = 0; i < n; ++i) {
        const f32 a = (f32)i * TAU / (f32)n + r.range(-0.3f, 0.3f);
        const f32 L = r.range(0.55f, 0.8f);
        auto at = [&](f32 t) {
            return glm::vec3(std::cos(a) * L * t, 0.45f * std::sin(t * 2.6f), std::sin(a) * L * t);
        };
        for (i32 k = 0; k < 4; ++k)
            s.segment(at((f32)k / 4.f), at((f32)(k + 1) / 4.f), 0.05f, col);
        for (f32 t : { 0.35f, 0.55f, 0.75f }) {
            const glm::vec3 c = at(t);
            const glm::vec3 perp{ -std::sin(a) * 0.12f, 0.f, std::cos(a) * 0.12f };
            s.segment(c - perp, c + perp, 0.035f, col);
        }
    }
    return std::move(s.m);
}

VoxelModel grass(Rng& r, u8 v, Grid vs) {
    const Palette& P = pal();
    Sculpt s(0.45f, 0.8f, vs);
    // Третий вариант — выгоревшая трава: её кладут на сухую землю.
    const u32 base = v == 2 ? P.dry : P.grass;
    const i32 n = 7 + v;
    for (i32 i = 0; i < n; ++i) {
        const glm::vec3 b{ r.range(-0.2f, 0.2f), 0.f, r.range(-0.2f, 0.2f) };
        const f32 h = r.range(0.3f, 0.62f);
        const glm::vec3 tip = b + glm::vec3(r.range(-0.15f, 0.15f), h, r.range(-0.15f, 0.15f));
        s.segment(b, tip, 0.04f, [&](const glm::vec3& p) {
            return p.y < h * 0.45f ? shade(base, 0.84f) : (p.y > h * 0.8f ? shade(base, 1.1f) : base);
        });
    }
    return std::move(s.m);
}

VoxelModel tallGrass(Rng& r, u8 v, Grid vs) {
    const Palette& P = pal();
    Sculpt s(0.5f, 1.3f, vs);
    const u32 base = v == 1 ? rgb(196, 176, 110) : P.grass;
    const u32 seed = rgb(170, 140, 90);
    for (i32 i = 0; i < 9; ++i) {
        const glm::vec3 b{ r.range(-0.22f, 0.22f), 0.f, r.range(-0.22f, 0.22f) };
        const f32 h = r.range(0.6f, 1.1f);
        const glm::vec3 tip = b + glm::vec3(r.range(-0.25f, 0.25f), h, r.range(-0.25f, 0.25f));
        s.segment(b, tip, 0.04f, [&](const glm::vec3& p) {
            return p.y < h * 0.4f ? shade(base, 0.84f) : base;
        });
        if (v == 1 && r.next() < 0.6f)
            s.ellipsoid(tip, { 0.04f, 0.1f, 0.04f }, [&](const glm::vec3&) { return seed; });
    }
    return std::move(s.m);
}

VoxelModel flowers(Rng& r, u8 v, Grid vs) {
    const Palette& P = pal();
    Sculpt s(0.4f, 0.6f, vs);
    static const u32 PETAL[4] = { rgb(214, 58, 52), rgb(236, 200, 56), rgb(236, 236, 228),
                                  rgb(138, 96, 196) };
    const u32 petal = PETAL[v & 3u], stem = rgb(86, 150, 64);
    for (i32 i = 0; i < 3; ++i) {
        const glm::vec3 b{ r.range(-0.2f, 0.2f), 0.f, r.range(-0.2f, 0.2f) };
        s.segment(b, b + glm::vec3(r.range(-0.1f, 0.1f), r.range(0.18f, 0.28f), r.range(-0.1f, 0.1f)),
                  0.035f, [&](const glm::vec3&) { return P.grass; });
    }
    const i32 n = 3 + (i32)(r.next() * 3.f);
    for (i32 i = 0; i < n; ++i) {
        const glm::vec3 b{ r.range(-0.18f, 0.18f), 0.f, r.range(-0.18f, 0.18f) };
        const glm::vec3 top = b + glm::vec3(r.range(-0.06f, 0.06f), r.range(0.25f, 0.45f),
                                            r.range(-0.06f, 0.06f));
        s.segment(b, top, 0.035f, [&](const glm::vec3&) { return stem; });
        s.ellipsoid(top, { 0.09f, 0.05f, 0.09f }, [&](const glm::vec3&) { return petal; });
    }
    return std::move(s.m);
}

VoxelModel reeds(Rng& r, u8 v, Grid vs) {
    Sculpt s(0.45f, 1.7f, vs);
    const u32 stem = rgb(104, 138, 70), head = rgb(112, 74, 44);
    const i32 n = 7 + v;
    for (i32 i = 0; i < n; ++i) {
        const glm::vec3 b{ r.range(-0.25f, 0.25f), 0.f, r.range(-0.25f, 0.25f) };
        const f32 h = r.range(0.9f, 1.5f);
        const glm::vec3 lean{ r.range(-0.1f, 0.1f), h, r.range(-0.1f, 0.1f) };
        s.segment(b, b + lean, 0.035f, [&](const glm::vec3& p) {
            return p.y < h * 0.4f ? shade(stem, 0.84f) : stem;
        });
        if (r.next() < 0.5f) {
            const glm::vec3 t = b + lean * 0.78f;
            s.segment(t, b + lean * 0.93f, 0.06f, [&](const glm::vec3&) { return head; });
        }
    }
    return std::move(s.m);
}

VoxelModel pebbles(Rng& r, u8 v, Grid vs) {
    const Palette& P = pal();
    Sculpt s(0.5f, 0.25f, vs);
    const u32 base = v == 1 ? rgb(170, 150, 120) : P.stone;
    const i32 n = 3 + v * 2;
    for (i32 i = 0; i < n; ++i) {
        const f32 rad = r.range(0.07f, 0.15f);
        const u32 c = shade(base, r.range(0.8f, 1.05f));
        s.ellipsoid({ r.range(-0.35f, 0.35f), 0.05f, r.range(-0.35f, 0.35f) },
                    { rad, rad * 0.55f, rad * 0.9f }, [&](const glm::vec3&) { return c; });
    }
    return std::move(s.m);
}

VoxelModel rock(Rng& r, u8 v, Grid vs) {
    const Palette& P = pal();
    Sculpt s(0.6f, 0.8f, vs);
    const u32 moss = rgb(98, 130, 70);
    auto col = [&](const glm::vec3& p) {
        if (v == 1 && p.y > 0.4f) return mottle(vs, moss, p, 61u, 0.08f, 0.25f);
        const u32 c = ((i32)std::floor(p.y / 0.125f) & 1) ? shade(P.stone, 0.94f) : P.stone;
        return mottle(vs, c, p, 67u, 0.06f, 0.25f);
    };
    if (v == 2) {   // плита
        s.ellipsoid({ 0.f, 0.08f, 0.f }, { 0.55f, 0.22f, 0.5f }, col);
        s.ellipsoid({ r.range(-0.3f, 0.3f), 0.05f, 0.35f }, { 0.18f, 0.15f, 0.16f }, col);
    } else {
        s.ellipsoid({ 0.f, 0.18f, 0.f }, { 0.5f, 0.42f, 0.44f }, col);
        for (i32 i = 0; i < 2; ++i) {
            const f32 a = r.range(0.f, TAU);
            s.ellipsoid({ std::cos(a) * 0.3f, r.range(0.1f, 0.3f), std::sin(a) * 0.3f },
                        { 0.25f, 0.25f, 0.22f }, col);
        }
    }
    return std::move(s.m);
}

VoxelModel fallenLog(Rng& r, u8 v, Grid vs) {
    const Palette& P = pal();
    Sculpt s(1.3f, 0.6f, vs);
    const f32 half = v == 1 ? 0.8f : 1.1f;
    const u32 moss = rgb(92, 128, 66);
    auto col = [&](const glm::vec3& p) {
        if (std::abs(p.x) > half - 0.07f) return P.cut;   // спил
        if (p.y > 0.36f && hash3((i32)std::floor(p.x / 0.25f), 0, (i32)std::floor(p.z / 0.25f), 71u) % 3u == 0u)
            return moss;
        return mottle(vs, P.bark, p, 73u, 0.07f, 0.25f);
    };
    s.segment({ -half, 0.22f, 0.f }, { half, 0.22f, 0.f }, 0.22f, col);
    if (v == 1)
        s.segment({ r.range(-0.3f, 0.3f), 0.35f, 0.f }, { 0.1f, 0.58f, 0.12f }, 0.07f,
                  [&](const glm::vec3&) { return P.bark; });
    return std::move(s.m);
}

VoxelModel stump(Rng& r, u8, Grid vs) {
    const Palette& P = pal();
    Sculpt s(0.5f, 0.5f, vs);
    s.cylinder(0.f, 0.f, 0.f, 0.38f, 0.3f, [&](const glm::vec3& p) {
        return p.y > 0.3f ? P.cut : mottle(vs, P.bark, p, 79u, 0.07f, 0.25f);
    });
    for (i32 i = 0; i < 3; ++i) {
        const f32 a = (f32)i * TAU / 3.f + r.range(-0.4f, 0.4f);
        s.segment({ 0.f, 0.1f, 0.f }, { std::cos(a) * 0.45f, 0.02f, std::sin(a) * 0.45f }, 0.07f,
                  [&](const glm::vec3&) { return P.bark; });
    }
    return std::move(s.m);
}

VoxelModel mushroom(Rng& r, u8 v, Grid vs) {
    Sculpt s(0.35f, 0.35f, vs);
    const u32 stem = rgb(230, 222, 206);
    const u32 cap = v == 0 ? rgb(200, 50, 40) : rgb(150, 104, 64);
    const i32 n = 2 + (i32)(r.next() * 2.f);
    for (i32 i = 0; i < n; ++i) {
        const glm::vec3 b{ r.range(-0.18f, 0.18f), 0.f, r.range(-0.18f, 0.18f) };
        const f32 h = r.range(0.1f, 0.2f);
        s.segment(b, b + glm::vec3(0.f, h, 0.f), 0.035f, [&](const glm::vec3&) { return stem; });
        s.ellipsoid(b + glm::vec3(0.f, h, 0.f), { 0.1f, 0.06f, 0.1f }, [&](const glm::vec3& p) {
            if (v == 0 && hash3((i32)std::floor(p.x / 0.125f), (i32)std::floor(p.y / 0.125f),
                                (i32)std::floor(p.z / 0.125f), 83u) % 4u == 0u)
                return stem;   // белые крапины
            return cap;
        });
    }
    return std::move(s.m);
}

u32 baseIndex(FloraKind k) {
    static const auto table = [] {
        std::array<u32, (usize)FloraKind::Count + 1> t{};
        for (u32 i = 0; i < (u32)FloraKind::Count; ++i) t[i + 1] = t[i] + world::FLORA_VARIANTS[i];
        return t;
    }();
    return table[(usize)k];
}

} // namespace

u32 floraModelIndex(FloraKind k, u8 variant) {
    return baseIndex(k) + (u32)(variant % world::FLORA_VARIANTS[(u32)k]);
}

u32 floraModelCount() { return baseIndex(FloraKind::Count); }

f32 floraVoxelSize(FloraKind k) {
    return (world::floraIsTree(k) && k != FloraKind::Cactus) ? 0.25f : 0.125f;
}

VoxelModel buildFloraModel(FloraKind k, u8 variant, u32 lod) {
    variant = (u8)(variant % world::FLORA_VARIANTS[(u32)k]);
    lod = std::min(lod, FLORA_LODS - 1);
    const f32 size = floraVoxelSize(k) * (f32)(1u << lod);
    const Grid vs{ size, lod > 0 ? size * 0.3f : 0.f };
    Rng r{ 0x5EED0000u ^ ((u32)k << 8) ^ variant };
    r.next();
    switch (k) {
        case FloraKind::Oak:       return oak(r, variant, vs);
        case FloraKind::Birch:     return birch(r, variant, vs);
        case FloraKind::Pine:      return pine(r, variant, vs);
        case FloraKind::Acacia:    return acacia(r, variant, vs);
        case FloraKind::Palm:      return palm(r, variant, vs);
        case FloraKind::DeadTree:  return deadTree(r, variant, vs);
        case FloraKind::Cactus:    return cactus(r, variant, vs);
        case FloraKind::Bush:      return bush(r, variant, vs);
        case FloraKind::DryBush:   return dryBush(r, variant, vs);
        case FloraKind::Fern:      return fern(r, variant, vs);
        case FloraKind::Grass:     return grass(r, variant, vs);
        case FloraKind::TallGrass: return tallGrass(r, variant, vs);
        case FloraKind::Flowers:   return flowers(r, variant, vs);
        case FloraKind::Reeds:     return reeds(r, variant, vs);
        case FloraKind::Pebbles:   return pebbles(r, variant, vs);
        case FloraKind::Rock:      return rock(r, variant, vs);
        case FloraKind::Log:       return fallenLog(r, variant, vs);
        case FloraKind::Stump:     return stump(r, variant, vs);
        case FloraKind::Mushroom:  return mushroom(r, variant, vs);
        case FloraKind::Count:     break;
    }
    return {};
}

std::vector<VoxelMesh> buildFloraMeshes() {
    std::vector<VoxelMesh> out(floraModelCount() * FLORA_LODS);
    for (u32 k = 0; k < (u32)FloraKind::Count; ++k)
        for (u8 v = 0; v < world::FLORA_VARIANTS[k]; ++v)
            for (u32 lod = 0; lod < FLORA_LODS; ++lod)
                out[floraModelIndex((FloraKind)k, v) * FLORA_LODS + lod] =
                    meshVoxelModel(buildFloraModel((FloraKind)k, v, lod));
    return out;
}

f32 floraSway(FloraKind k) {
    switch (k) {
        case FloraKind::Grass:
        case FloraKind::TallGrass: return 1.0f;
        case FloraKind::Flowers:   return 0.9f;
        case FloraKind::Reeds:     return 0.8f;
        case FloraKind::Fern:      return 0.6f;
        case FloraKind::DryBush:   return 0.4f;
        case FloraKind::Bush:      return 0.25f;
        case FloraKind::Palm:      return 0.08f;
        case FloraKind::Oak:
        case FloraKind::Birch:
        case FloraKind::Pine:
        case FloraKind::Acacia:
        case FloraKind::DeadTree:  return 0.04f;
        default:                   return 0.f;
    }
}

} // namespace render
