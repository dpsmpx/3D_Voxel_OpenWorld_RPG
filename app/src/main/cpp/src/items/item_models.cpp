/**
 * @file item_models.cpp
 * @brief Предметы: воксельные модели — как предмет выглядит в мире и на значке.
 */
#include "item_models.h"
#include "item_def.h"
#include "../world/block.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace items {

namespace {

using render::VoxelModel;

// ---- Позы для значка ----
//
// Стоящее (блок, склянка, яблоко) — в изометрии: сверху-сбоку, видны
// верх и две стороны. Лежащее плашмя (клинок, лук, руна) — почти
// сверху и наискосок, чтобы длинная ось шла из левого нижнего угла в
// правый верхний: так значок оружия читается с детства.
constexpr f32 SOLID_YAW   = -0.7853982f;
constexpr f32 SOLID_PITCH =  0.5235988f;
constexpr f32 FLAT_YAW    =  0.7853982f;
constexpr f32 FLAT_PITCH  =  1.2217305f;

/// Размер вокселя по умолчанию: шестнадцать вокселей — полблока.
constexpr f32 VOXEL = 1.f / 32.f;

u32 rgb(u8 r, u8 g, u8 b) {
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFFu;
}

/// Непрозрачный вариант цвета: у воды, льда и листвы в реестре
/// блоков альфа меньше единицы, а модель рисуется без смешивания.
u32 opaque(u32 c) { return c | 0xFFu; }

u32 shade(u32 c, f32 k) {
    auto ch = [&](u32 s) {
        const f32 v = (f32)((c >> s) & 0xFFu) * k;
        return (u32)std::clamp(v, 0.f, 255.f) << s;
    };
    return ch(24) | ch(16) | ch(8) | 0xFFu;
}

u32 mix(u32 a, u32 b, f32 t) {
    auto ch = [&](u32 s) {
        const f32 va = (f32)((a >> s) & 0xFFu), vb = (f32)((b >> s) & 0xFFu);
        return (u32)std::clamp(va + (vb - va) * t, 0.f, 255.f) << s;
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
f32 rnd(i32 x, i32 y, i32 z, u32 seed) {
    return (f32)(hash3(x, y, z, seed) & 0xFFFFu) / 65535.f;
}

/// Цвет с крапчатостью: живой материал не бывает однотонным, и без
/// неё модель из сотни вокселей читается заливкой.
u32 jit(u32 c, i32 x, i32 y, i32 z, u32 seed, f32 amp) {
    return amp > 0.f ? shade(c, 1.f + (rnd(x, y, z, seed) * 2.f - 1.f) * amp) : c;
}

/// Построитель: заливки коробкой и шаром, с крапчатостью.
struct B {
    VoxelModel& m;
    u32 seed;

    void put(i32 x, i32 y, i32 z, u32 c, f32 amp = 0.f) {
        m.set(x, y, z, jit(c, x, y, z, seed, amp));
    }
    void erase(i32 x, i32 y, i32 z) { m.set(x, y, z, 0u); }
    /// Коробка, границы включительно.
    void box(i32 x0, i32 y0, i32 z0, i32 x1, i32 y1, i32 z1, u32 c, f32 amp = 0.f) {
        for (i32 y = y0; y <= y1; ++y)
            for (i32 z = z0; z <= z1; ++z)
                for (i32 x = x0; x <= x1; ++x) put(x, y, z, c, amp);
    }
    /// Эллипсоид: центр и полуоси в вокселях.
    template<typename F>
    void blob(f32 cx, f32 cy, f32 cz, f32 rx, f32 ry, f32 rz, F color) {
        for (i32 y = 0; y < m.sy; ++y)
            for (i32 z = 0; z < m.sz; ++z)
                for (i32 x = 0; x < m.sx; ++x) {
                    const f32 dx = ((f32)x + 0.5f - cx) / rx;
                    const f32 dy = ((f32)y + 0.5f - cy) / ry;
                    const f32 dz = ((f32)z + 0.5f - cz) / rz;
                    if (dx * dx + dy * dy + dz * dz <= 1.f) m.set(x, y, z, color(x, y, z));
                }
    }
};

VoxelModel make(i32 sx, i32 sy, i32 sz, bool flat, f32 voxel = VOXEL) {
    VoxelModel m;
    m.resize(sx, sy, sz);
    m.voxelSize = voxel;
    m.iconYaw   = flat ? FLAT_YAW : SOLID_YAW;
    m.iconPitch = flat ? FLAT_PITCH : SOLID_PITCH;
    return m;
}

// ============================================================
// Блоки: блок в миниатюре с узором своего материала
// ============================================================
VoxelModel miniBlock(u16 blockId) {
    const world::BlockDef& d = world::blocks().get(blockId);
    constexpr i32 N = 10;
    VoxelModel m = make(N, N, N, false);
    B b{ m, 0xB10C0000u + blockId };
    const u32 top = opaque(d.colorTop), side = opaque(d.colorSide), bot = opaque(d.colorBottom);

    auto surface = [&](i32 x, i32 y, i32 z) {
        return x == 0 || y == 0 || z == 0 || x == N - 1 || y == N - 1 || z == N - 1;
    };

    for (i32 y = 0; y < N; ++y)
        for (i32 z = 0; z < N; ++z)
            for (i32 x = 0; x < N; ++x) {
                u32 c = y == N - 1 ? top : (y == 0 ? bot : side);
                f32 amp = 0.07f;
                const f32 r = rnd(x, y, z, b.seed);
                switch (blockId) {
                    case world::GRASS: {
                        // Дёрн свисает с верха неровной бахромой.
                        const f32 fringe = y == N - 2 ? 0.55f : (y == N - 3 ? 0.15f : 0.f);
                        if (y < N - 1 && r < fringe) c = top;
                        amp = 0.09f;
                        break;
                    }
                    case world::WOOD: {
                        const f32 dx = (f32)x - 4.5f, dz = (f32)z - 4.5f;
                        const f32 rad = std::sqrt(dx * dx + dz * dz);
                        if (y == 0 || y == N - 1) {
                            // Спил: годовые кольца, по краю кора.
                            c = rad > 4.2f ? side : ((i32)rad % 2 ? shade(top, 0.86f) : top);
                        } else if ((x + z) % 3 == 0) {
                            c = shade(side, 0.84f);   // борозды коры
                        }
                        amp = 0.05f;
                        break;
                    }
                    case world::LEAVES:
                        // Крона: пятнистая и с дырами по поверхности.
                        if (surface(x, y, z) && r < 0.18f) { c = 0u; break; }
                        amp = 0.16f;
                        break;
                    case world::IRON_ORE:
                    case world::GOLD_ORE: {
                        // Порода с вкраплениями металла гнёздами 2×2.
                        const u32 stone = opaque(world::blocks().get(world::STONE).colorSide);
                        const bool vein = rnd(x / 2, y / 2, z / 2, b.seed ^ 0x5EEDu) < 0.28f;
                        c = vein ? top : stone;
                        amp = vein ? 0.10f : 0.06f;
                        break;
                    }
                    case world::CACTUS:
                        if (y < N - 1 && (x % 3 == 0 || z % 3 == 0)) c = shade(side, 0.80f);
                        if (surface(x, y, z) && r < 0.05f) c = rgb(236, 228, 176);   // колючки
                        amp = 0.05f;
                        break;
                    case world::BRICK: {
                        // Кладка: ряды по три, швы вразбежку.
                        const i32 row = y / 3;
                        const bool mortar = y % 3 == 2 || ((x + z + (row % 2) * 3) % 6 == 5);
                        c = mortar ? rgb(196, 188, 172) : side;
                        amp = mortar ? 0.03f : 0.08f;
                        break;
                    }
                    case world::ICE:
                        if ((x + 2 * y + z) % 7 == 0) c = shade(c, 1.12f);
                        amp = 0.04f;
                        break;
                    case world::SNOW:
                        amp = 0.03f;
                        break;
                    case world::DIRT:
                    case world::SAND:
                        if (r < 0.05f) c = shade(c, 0.78f);   // камешки
                        break;
                    default:
                        break;
                }
                if (c) b.put(x, y, z, c, amp);
            }
    return m;
}

/// Факел: стоящая палка с намотанной паклей и пламенем.
VoxelModel torch() {
    VoxelModel m = make(5, 13, 5, false);
    B b{ m, 0x70C4u };
    b.box(2, 0, 2, 2, 8, 2, rgb(126, 88, 52), 0.08f);
    b.box(1, 8, 1, 3, 9, 3, rgb(70, 62, 58), 0.10f);
    b.box(1, 10, 1, 3, 10, 3, rgb(255, 128, 36));
    b.box(2, 11, 1, 2, 11, 3, rgb(255, 196, 64));
    b.box(1, 11, 2, 3, 11, 2, rgb(255, 196, 64));
    b.put(2, 12, 2, rgb(255, 244, 190));
    return m;
}

// ============================================================
// Оружие: лежит плашмя, длинная ось — X
// ============================================================
constexpr u32 STEEL      = 0xC4CAD2FFu;
constexpr u32 STEEL_EDGE = 0xE6EBF0FFu;
constexpr u32 IRON_DARK  = 0x6E7480FFu;
constexpr u32 GOLDEN     = 0xE0B040FFu;
constexpr u32 LEATHER    = 0x6A4428FFu;
constexpr u32 WOOD_DARK  = 0x6E4A2AFFu;
constexpr u32 WOOD_LIGHT = 0xA8784AFFu;

/// Клинок с гардой и рукоятью: меч — длинный, кинжал — короткий.
VoxelModel blade(i32 bladeLen, u32 guardColor) {
    const i32 sx = 5 + bladeLen + 2;
    VoxelModel m = make(sx, 2, 5, true);
    B b{ m, 0x5A0Du + (u32)bladeLen };
    b.box(0, 0, 1, 0, 1, 3, guardColor, 0.05f);                 // навершие
    for (i32 x = 1; x <= 3; ++x)                                // рукоять в обмотке
        b.box(x, 0, 2, x, 1, 2, x % 2 ? LEATHER : shade(LEATHER, 0.8f));
    b.box(4, 0, 0, 4, 1, 4, guardColor, 0.05f);                 // гарда
    for (i32 x = 5; x < 5 + bladeLen; ++x) {
        b.put(x, 0, 1, STEEL_EDGE, 0.03f);
        b.put(x, 0, 2, shade(STEEL, 0.9f), 0.03f);              // дол
        b.put(x, 0, 3, STEEL_EDGE, 0.03f);
    }
    b.put(5 + bladeLen, 0, 2, STEEL, 0.03f);
    b.put(6 + bladeLen, 0, 2, STEEL_EDGE);                      // остриё
    return m;
}

VoxelModel throwingKnife() {
    VoxelModel m = make(8, 1, 3, true);
    B b{ m, 0x7E1Fu };
    b.box(0, 0, 0, 0, 0, 2, IRON_DARK);                          // кольцо
    b.put(1, 0, 0, IRON_DARK); b.put(1, 0, 2, IRON_DARK);
    b.put(2, 0, 1, LEATHER);
    b.box(3, 0, 1, 6, 0, 1, STEEL, 0.03f);
    b.box(3, 0, 0, 5, 0, 0, STEEL_EDGE);
    b.put(7, 0, 1, STEEL_EDGE);
    return m;
}

VoxelModel axe() {
    VoxelModel m = make(15, 2, 7, true);
    B b{ m, 0xA7Eu };
    for (i32 x = 0; x <= 13; ++x)
        b.box(x, 0, 3, x, 1, 3, x <= 3 ? shade(WOOD_LIGHT, 0.75f) : WOOD_LIGHT, 0.06f);
    b.box(10, 0, 2, 12, 1, 4, IRON_DARK, 0.05f);                // обух
    b.box(9, 0, 1, 14, 0, 1, STEEL, 0.04f);                     // лезвие
    b.box(10, 0, 0, 13, 0, 0, STEEL_EDGE);
    b.erase(9, 0, 1); b.erase(14, 0, 1);
    b.put(9, 0, 2, STEEL); b.put(13, 0, 2, STEEL);
    b.put(11, 0, 5, IRON_DARK); b.put(11, 0, 6, IRON_DARK);     // шип
    return m;
}

VoxelModel spear() {
    VoxelModel m = make(16, 1, 3, true);
    B b{ m, 0x5BEAu };
    for (i32 x = 0; x <= 11; ++x)
        b.put(x, 0, 1, (x == 4 || x == 8) ? WOOD_DARK : WOOD_LIGHT, 0.06f);
    b.box(12, 0, 0, 12, 0, 2, IRON_DARK);
    b.box(13, 0, 0, 14, 0, 2, STEEL, 0.03f);
    b.put(13, 0, 1, shade(STEEL, 0.85f)); b.put(14, 0, 1, shade(STEEL, 0.85f));
    b.put(15, 0, 1, STEEL_EDGE);
    return m;
}

VoxelModel bow() {
    VoxelModel m = make(5, 1, 15, true);
    B b{ m, 0xB0Bu };
    for (i32 z = 0; z < 15; ++z) {
        const i32 x = (i32)std::lround(4.f * std::sin(3.14159265f * (f32)z / 14.f));
        const bool grip = z >= 6 && z <= 8;
        b.put(x, 0, z, grip ? LEATHER : WOOD_LIGHT, 0.06f);
        if (z > 0 && z < 14 && x > 0) b.put(0, 0, z, rgb(224, 220, 206));   // тетива
    }
    b.put(0, 0, 0, WOOD_DARK); b.put(0, 0, 14, WOOD_DARK);
    return m;
}

VoxelModel crossbow() {
    VoxelModel m = make(14, 2, 11, true);
    B b{ m, 0xC205u };
    for (i32 x = 0; x <= 13; ++x) b.box(x, 0, 5, x, 1, 5, WOOD_DARK, 0.06f);
    b.box(0, 0, 4, 2, 1, 6, shade(WOOD_DARK, 0.85f), 0.06f);    // приклад
    for (i32 z = 0; z <= 10; ++z) {
        const i32 off = std::abs(z - 5) >= 4 ? -1 : 0;
        b.put(11 + off, 0, z, IRON_DARK, 0.05f);                // дуга
    }
    for (i32 z = 1; z <= 9; ++z) {                               // тетива
        const i32 x = 10 - (5 - std::abs(z - 5)) * 3 / 5;
        if (z != 5) b.put(x, 1, z, rgb(224, 220, 206));
    }
    b.put(4, 1, 5, IRON_DARK);                                   // спуск
    return m;
}

/// Посох и палочка: древко и камень на конце.
VoxelModel staff(u32 shaft, u32 band, u32 gemCore, u32 gemEdge, i32 len, bool crystal) {
    VoxelModel m = make(len + 4, 3, 3, true);
    B b{ m, 0x57AFu + (u32)len };
    for (i32 x = 0; x < len; ++x)
        b.put(x, 1, 1, (x == 2 || x == len - 3) ? band : shade(shaft, x < 3 ? 0.8f : 1.f), 0.05f);
    if (crystal) {
        // Кристалл — ромб, вытянутый вдоль древка.
        b.put(len, 1, 1, gemEdge);
        b.box(len + 1, 0, 1, len + 2, 2, 1, gemEdge);
        b.box(len + 1, 1, 0, len + 2, 1, 2, gemEdge);
        b.put(len + 1, 1, 1, gemCore); b.put(len + 2, 1, 1, gemCore);
        b.put(len + 3, 1, 1, rgb(240, 252, 255));
    } else {
        // Самоцвет в золотых лапках.
        b.box(len, 0, 0, len, 2, 2, GOLDEN, 0.04f);
        b.erase(len, 1, 1);
        b.box(len + 1, 0, 0, len + 3, 2, 2, gemEdge, 0.06f);
        b.erase(len + 1, 0, 0); b.erase(len + 1, 2, 2);
        b.erase(len + 3, 0, 2); b.erase(len + 3, 2, 0);
        b.box(len + 2, 1, 1, len + 2, 2, 1, gemCore);
    }
    return m;
}

VoxelModel bracelet() {
    VoxelModel m = make(12, 2, 12, true);
    B b{ m, 0xB2ACu };
    const u32 band = rgb(122, 79, 200);
    for (i32 z = 0; z < 12; ++z)
        for (i32 x = 0; x < 12; ++x) {
            const f32 dx = (f32)x + 0.5f - 6.f, dz = (f32)z + 0.5f - 6.f;
            const f32 r = std::sqrt(dx * dx + dz * dz);
            if (r < 4.f || r >= 5.9f) continue;
            b.put(x, 0, z, band, 0.07f);
            b.put(x, 1, z, r > 5.f ? GOLDEN : band, 0.07f);
        }
    b.box(5, 0, 0, 6, 1, 1, rgb(232, 64, 190));                   // самоцвет
    b.put(5, 1, 0, rgb(255, 170, 235));
    return m;
}

// ============================================================
// Руны: каменная плитка с горящим знаком
// ============================================================
VoxelModel rune(u32 glow, bool ice) {
    VoxelModel m = make(8, 2, 10, true);
    // Плитку держат прямо, как табличку, а не ромбом: знак на ней
    // должен читаться.
    m.iconYaw = 0.3f;
    m.iconPitch = 1.05f;
    B b{ m, ice ? 0x1CEu : 0xF1AEu };
    b.box(0, 0, 0, 7, 0, 9, rgb(112, 114, 124), 0.10f);
    b.erase(0, 0, 0); b.erase(7, 0, 0); b.erase(0, 0, 9); b.erase(7, 0, 9);
    // Знак 6×8 поверх плитки: '#' — горит.
    static const char* ICE_GLYPH[8] = {
        "..#...", ".###..", "#.#.#.", "..#...", "..#...", "#.#.#.", ".###..", "..#...",
    };
    static const char* FIRE_GLYPH[8] = {
        "..#...", "..##..", ".###..", ".##.#.", "#.####", "######", ".####.", "..##..",
    };
    const char* const* g = ice ? ICE_GLYPH : FIRE_GLYPH;
    for (i32 row = 0; row < 8; ++row)
        for (i32 col = 0; col < 6; ++col)
            if (g[row][col] == '#')
                b.put(1 + col, 1, 1 + row, row % 3 == 0 ? shade(glow, 1.15f) : glow);
    return m;
}

// ============================================================
// Метательное
// ============================================================
VoxelModel shuriken() {
    VoxelModel m = make(9, 1, 9, true);
    B b{ m, 0x5B1Eu };
    b.box(3, 0, 3, 5, 0, 5, STEEL, 0.04f);
    b.erase(4, 0, 4);
    // Лопасть «мельницей» и три её поворота.
    const i32 blade[6][2] = { {0, -4}, {0, -3}, {1, -3}, {1, -2}, {0, -2}, {-1, -2} };
    for (int r = 0; r < 4; ++r)
        for (const auto& p : blade) {
            i32 dx = p[0], dz = p[1];
            for (int k = 0; k < r; ++k) { const i32 t = dx; dx = -dz; dz = t; }
            b.put(4 + dx, 0, 4 + dz, (dx == 0 || dz == 0) ? STEEL_EDGE : STEEL, 0.03f);
        }
    return m;
}

VoxelModel trampoline() {
    VoxelModel m = make(13, 3, 13, false);
    B b{ m, 0x7A3Bu };
    const i32 legs[4][2] = { {2, 2}, {10, 2}, {2, 10}, {10, 10} };
    for (const auto& l : legs) b.put(l[0], 0, l[1], rgb(58, 58, 66));
    for (i32 z = 0; z < 13; ++z)
        for (i32 x = 0; x < 13; ++x) {
            const f32 dx = (f32)x + 0.5f - 6.5f, dz = (f32)z + 0.5f - 6.5f;
            const f32 r = std::sqrt(dx * dx + dz * dz);
            if (r >= 6.5f) continue;
            if (r >= 5.f && r < 6.5f) b.put(x, 1, z, rgb(80, 86, 104), 0.05f);
            if (r >= 5.2f) b.put(x, 2, z, ((x + z) % 4 < 2) ? rgb(230, 70, 60) : rgb(250, 210, 70));
            else b.put(x, 2, z, r < 1.6f ? rgb(90, 150, 240) : rgb(46, 110, 216), 0.05f);
        }
    return m;
}

// ============================================================
// Зелья: склянка с жидкостью своего цвета
// ============================================================
constexpr u32 GLASS = 0xCFE6EEFFu;
constexpr u32 CORK  = 0x9A6A3AFFu;

VoxelModel potion(u32 liquid, bool large) {
    const i32 n = large ? 9 : 7;
    const i32 h = large ? 13 : 10;
    VoxelModel m = make(n, h, n, false);
    B b{ m, liquid ^ (large ? 0x1A2Bu : 0x5A1Bu) };
    const f32 c = (f32)n * 0.5f;
    const f32 r = large ? 4.3f : 3.3f;
    const f32 cy = r - 0.3f;
    b.blob(c, cy, c, r, r, r, [&](i32 x, i32 y, i32 z) {
        // Блик стекла слева сверху, верхний слой — стекло над жидкостью.
        if ((f32)y > cy + r * 0.55f) return GLASS;
        if ((f32)x < c - r * 0.45f && (f32)y > cy) return mix(liquid, 0xFFFFFFFFu, 0.45f);
        return jit(liquid, x, y, z, b.seed, 0.06f);
    });
    const i32 neckY = (i32)(cy + r);
    const i32 mid = n / 2;
    b.box(mid - 1, neckY, mid - 1, mid + 1, neckY + 1, mid + 1, GLASS, 0.03f);
    if (large) b.box(mid - 2, neckY + 2, mid - 2, mid + 2, neckY + 2, mid + 2, GLASS, 0.03f);
    const i32 corkY = neckY + (large ? 3 : 2);
    b.box(mid - 1, corkY, mid - 1, mid + 1, std::min(h - 1, corkY + 1), mid + 1, CORK, 0.08f);
    return m;
}

/// Эликсир: высокая узкая склянка с золотым пояском и сургучом.
VoxelModel elixir(u32 liquid) {
    VoxelModel m = make(7, 12, 7, false);
    B b{ m, liquid ^ 0xE11Eu };
    for (i32 y = 0; y <= 6; ++y)
        for (i32 z = 0; z < 7; ++z)
            for (i32 x = 0; x < 7; ++x) {
                const f32 dx = (f32)x + 0.5f - 3.5f, dz = (f32)z + 0.5f - 3.5f;
                if (dx * dx + dz * dz > 2.7f * 2.7f) continue;
                u32 col = y == 2 ? GOLDEN : jit(liquid, x, y, z, b.seed, 0.06f);
                if (y != 2 && dx < -1.2f && y >= 3) col = mix(liquid, 0xFFFFFFFFu, 0.4f);
                b.put(x, y, z, col);
            }
    b.box(2, 7, 2, 4, 7, 4, GLASS);
    b.box(3, 8, 3, 3, 9, 3, GLASS);
    b.box(2, 10, 2, 4, 10, 4, rgb(150, 30, 40));
    b.put(3, 11, 3, rgb(180, 40, 50));
    return m;
}

// ============================================================
// Еда
// ============================================================
VoxelModel bread() {
    VoxelModel m = make(12, 6, 7, false);
    m.iconYaw = -0.55f;
    B b{ m, 0xB2EAu };
    b.blob(6.f, 0.5f, 3.5f, 6.1f, 5.6f, 3.6f, [&](i32 x, i32 y, i32 z) {
        const bool cut = y >= 4 && (x == 3 || x == 6 || x == 9);
        if (cut) return rgb(234, 210, 160);
        return jit(y >= 3 ? rgb(184, 116, 46) : rgb(217, 162, 90), x, y, z, b.seed, 0.07f);
    });
    return m;
}

VoxelModel meat(bool cooked) {
    VoxelModel m = make(11, 3, 8, false);
    m.iconYaw = -0.55f;
    m.iconPitch = 0.75f;
    B b{ m, cooked ? 0xC00Cu : 0x5A3Eu };
    const u32 flesh = cooked ? rgb(138, 74, 38) : rgb(200, 72, 78);
    const u32 fat   = cooked ? rgb(200, 144, 72) : rgb(240, 224, 200);
    for (i32 y = 0; y < 3; ++y)
        for (i32 z = 0; z < 8; ++z)
            for (i32 x = 0; x < 11; ++x) {
                const f32 dx = ((f32)x + 0.5f - 5.f) / 5.f;
                const f32 dz = ((f32)z + 0.5f - 4.f) / 3.8f;
                const f32 d = dx * dx + dz * dz;
                const f32 lim = y == 2 ? 0.7f : 1.f;
                if (d > lim) continue;
                u32 col = d > lim * 0.72f ? fat : flesh;
                if (cooked && y == 2 && (x + z) % 4 == 0) col = rgb(74, 36, 16);   // решётка
                if (!cooked && d <= lim * 0.72f && rnd(x, y, z, b.seed) < 0.15f)
                    col = rgb(232, 150, 150);                                    // мраморность
                b.put(x, y, z, col, 0.05f);
            }
    b.box(9, 0, 3, 10, 1, 4, rgb(236, 230, 214));               // косточка
    return m;
}

VoxelModel apple() {
    VoxelModel m = make(9, 10, 9, false);
    B b{ m, 0xA991u };
    b.blob(4.5f, 4.f, 4.5f, 4.4f, 4.f, 4.4f, [&](i32 x, i32 y, i32 z) {
        u32 c = rgb(200, 36, 44);
        if (x <= 2 && y >= 5) c = rgb(236, 90, 84);             // блик
        if (y <= 1) c = shade(c, 0.8f);
        return jit(c, x, y, z, b.seed, 0.06f);
    });
    b.erase(4, 7, 4);                                           // ямка у черенка
    b.box(4, 7, 4, 4, 9, 4, rgb(96, 62, 30));
    b.box(5, 8, 4, 6, 8, 4, rgb(72, 160, 60));
    b.put(6, 9, 4, rgb(96, 186, 72));
    return m;
}

// ============================================================
// Материалы и деньги
// ============================================================
VoxelModel ingot(u32 metal) {
    VoxelModel m = make(10, 3, 5, false);
    B b{ m, metal ^ 0x16u };
    b.box(0, 0, 0, 9, 0, 4, shade(metal, 0.82f), 0.04f);
    b.box(1, 1, 0, 8, 1, 4, shade(metal, 0.92f), 0.04f);
    b.box(1, 2, 1, 8, 2, 3, shade(metal, 1.08f), 0.03f);
    return m;
}

VoxelModel leatherHide() {
    VoxelModel m = make(12, 1, 10, true);
    B b{ m, 0x1EA7u };
    for (i32 z = 0; z < 10; ++z)
        for (i32 x = 0; x < 12; ++x) {
            const f32 dx = ((f32)x + 0.5f - 6.f) / 6.f;
            const f32 dz = ((f32)z + 0.5f - 5.f) / 5.f;
            const f32 d = dx * dx + dz * dz;
            if (d > 1.f + (rnd(x, 0, z, b.seed) - 0.5f) * 0.35f) continue;
            b.put(x, 0, z, d > 0.7f ? rgb(112, 70, 38) : rgb(150, 98, 56), 0.10f);
        }
    return m;
}

VoxelModel bone() {
    VoxelModel m = make(12, 2, 4, true);
    B b{ m, 0xB04Eu };
    const u32 ivory = rgb(232, 224, 200);
    b.box(2, 0, 1, 9, 1, 2, ivory, 0.05f);
    for (i32 x : { 0, 10 }) {
        b.box(x, 0, 0, x + 1, 1, 1, ivory, 0.06f);
        b.box(x, 0, 2, x + 1, 1, 3, ivory, 0.06f);
    }
    return m;
}

VoxelModel cloth() {
    VoxelModel m = make(10, 3, 8, false);
    B b{ m, 0xC107u };
    const u32 base = rgb(224, 210, 176), stripe = rgb(176, 48, 42);
    for (i32 y = 0; y < 3; ++y) {
        const i32 x0 = y == 2 ? 1 : 0, x1 = y == 0 ? 9 : 8;
        for (i32 z = 0; z < 8; ++z)
            for (i32 x = x0; x <= x1; ++x)
                b.put(x, y, z, (z == 2 || z == 5) ? stripe : base, 0.04f);
    }
    return m;
}

VoxelModel coins() {
    // Стопка монет и одна монета рядом: одна кучка читалась слитком.
    VoxelModel m = make(13, 5, 9, false);
    m.iconPitch = 0.62f;
    B b{ m, 0xC014u };
    auto coin = [&](f32 cx, f32 cz, i32 y, f32 rad, bool face) {
        for (i32 z = 0; z < 9; ++z)
            for (i32 x = 0; x < 13; ++x) {
                const f32 dx = (f32)x + 0.5f - cx, dz = (f32)z + 0.5f - cz;
                const f32 r = std::sqrt(dx * dx + dz * dz);
                if (r > rad) continue;
                // Ребро у каждой монеты стопки — через одну темнее:
                // так видно, что монет несколько.
                u32 c = r > rad - 0.9f ? (y % 2 ? rgb(176, 128, 34) : rgb(206, 156, 44))
                                       : rgb(240, 196, 70);
                if (face && r < 1.2f) c = rgb(255, 234, 150);
                b.put(x, y, z, c, 0.04f);
            }
    };
    for (i32 y = 0; y < 5; ++y) coin(4.5f, 4.5f, y, 3.4f, y == 4);
    coin(10.3f, 5.f, 0, 2.6f, true);
    return m;
}

/// Запасной вид: кубик цвета редкости. Предметов с ним нет.
VoxelModel fallback(u16 itemId) {
    VoxelModel m = make(6, 6, 6, false);
    B b{ m, itemId };
    b.box(0, 0, 0, 5, 5, 5, opaque(rarityColor(items().rarity(itemId))), 0.06f);
    return m;
}

bool buildModel(u16 id, VoxelModel& out) {
    const ItemDef& def = items().get(id);
    if (!def.name) return false;

    if (def.category == ItemCategory::Block) {
        if (def.payload.blockId == world::TORCH) out = torch();
        else out = miniBlock(def.payload.blockId);
        return true;
    }

    // Цвета жидкостей зелий: здоровье, мана, выносливость, эликсиры.
    constexpr u32 RED = 0xD8283AFFu, BLUE = 0x2F6BE8FFu, GREEN = 0x4CC04AFFu;

    switch (id) {
        case ITEM_IRON_INGOT:        out = ingot(0xB8BCC4FFu); return true;
        case ITEM_GOLD_INGOT:        out = ingot(0xE8B83AFFu); return true;
        case ITEM_LEATHER:           out = leatherHide();      return true;
        case ITEM_BONE:              out = bone();             return true;
        case ITEM_CLOTH:             out = cloth();            return true;

        case ITEM_TRAMPOLINE:        out = trampoline();       return true;
        case ITEM_SHURIKEN:          out = shuriken();         return true;

        case ITEM_IRON_SWORD:        out = blade(9, IRON_DARK);  return true;
        case ITEM_IRON_DAGGER:       out = blade(4, IRON_DARK);  return true;
        case ITEM_IRON_AXE:          out = axe();              return true;
        case ITEM_IRON_SPEAR:        out = spear();            return true;
        case ITEM_HUNTING_BOW:       out = bow();              return true;
        case ITEM_HEAVY_CROSSBOW:    out = crossbow();         return true;
        case ITEM_THROWING_KNIFE:    out = throwingKnife();    return true;
        case ITEM_FIRE_STAFF:
            out = staff(WOOD_DARK, GOLDEN, rgb(255, 214, 90), rgb(232, 72, 30), 12, false);
            return true;
        case ITEM_FROST_WAND:
            out = staff(rgb(196, 204, 214), rgb(70, 110, 190), rgb(214, 246, 255),
                        rgb(120, 200, 240), 9, true);
            return true;
        case ITEM_ARCANE_BRACELET:   out = bracelet();         return true;

        case ITEM_RUNE_ICE_NEEDLES:  out = rune(rgb(150, 230, 255), true);  return true;
        case ITEM_RUNE_FLAME:        out = rune(rgb(255, 130, 40), false);  return true;

        case ITEM_POTION_HEALTH_SMALL: out = potion(RED, false);   return true;
        case ITEM_POTION_HEALTH_LARGE: out = potion(RED, true);    return true;
        case ITEM_POTION_MANA_SMALL:   out = potion(BLUE, false);  return true;
        case ITEM_POTION_MANA_LARGE:   out = potion(BLUE, true);   return true;
        case ITEM_POTION_STAMINA:      out = potion(GREEN, false); return true;
        case ITEM_ELIXIR_STRENGTH:     out = elixir(rgb(232, 106, 32));  return true;
        case ITEM_ELIXIR_AGILITY:      out = elixir(rgb(70, 208, 138));  return true;
        case ITEM_ELIXIR_INTELLECT:    out = elixir(rgb(138, 79, 224));  return true;
        case ITEM_ELIXIR_ENDURANCE:    out = elixir(rgb(200, 160, 64));  return true;

        case ITEM_BREAD:             out = bread();            return true;
        case ITEM_MEAT_RAW:          out = meat(false);        return true;
        case ITEM_MEAT_COOKED:       out = meat(true);         return true;
        case ITEM_APPLE:             out = apple();            return true;

        case ITEM_GOLD_COIN:         out = coins();            return true;
        default:
            return false;
    }
}

struct ModelTable {
    std::vector<VoxelModel> models;
    std::vector<u8> own;
    ModelTable() : models(ITEM_MAX_DEFS), own(ITEM_MAX_DEFS, 0) {
        // Пустым номерам модель не нужна вовсе: строить им запасной
        // кубик — сотни моделей впустую на старте.
        for (u16 id = 0; id < ITEM_MAX_DEFS; ++id) {
            if (!items().get(id).name) continue;
            if (buildModel(id, models[id])) own[id] = 1;
            else models[id] = fallback(id);
        }
    }
};

const ModelTable& table() {
    static const ModelTable t;
    return t;
}

} // namespace

const render::VoxelModel& itemModel(u16 itemId) {
    const ModelTable& t = table();
    return t.models[itemId < ITEM_MAX_DEFS ? itemId : 0];
}

bool hasOwnModel(u16 itemId) {
    return itemId < ITEM_MAX_DEFS && table().own[itemId] != 0;
}

} // namespace items
