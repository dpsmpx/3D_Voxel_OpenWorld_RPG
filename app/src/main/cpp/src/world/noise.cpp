/**
 * @file noise.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "noise.h"
#include <algorithm>

namespace world {

namespace {

// SplitMix64 — быстрый и качественный смеситель для инициализации
// перестановки по 64-битному seed.
inline u64 splitmix64(u64& state) {
    u64 z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// 12 рёбер куба — стандартный набор градиентов Simplex 3D.
constexpr i8 GRAD3[12][3] = {
    { 1, 1, 0}, {-1, 1, 0}, { 1,-1, 0}, {-1,-1, 0},
    { 1, 0, 1}, {-1, 0, 1}, { 1, 0,-1}, {-1, 0,-1},
    { 0, 1, 1}, { 0,-1, 1}, { 0, 1,-1}, { 0,-1,-1},
};

inline i32 fastFloor(f32 v) {
    const i32 i = (i32)v;
    return v < (f32)i ? i - 1 : i;
}

} // namespace

SimplexNoise::SimplexNoise(u64 seed) {
    // Перестановка 0..255, перемешанная Fisher–Yates по seed.
    u8 p[256];
    for (i32 i = 0; i < 256; ++i) p[i] = (u8)i;

    u64 state = seed ^ 0xDEADBEEFCAFEBABEULL;
    for (i32 i = 255; i > 0; --i) {
        const u32 j = (u32)(splitmix64(state) % (u64)(i + 1));
        std::swap(p[i], p[j]);
    }
    // Удвоение снимает необходимость в маскировании индексов.
    for (i32 i = 0; i < 512; ++i) perm_[i] = p[i & 255];
}

f32 SimplexNoise::grad(i32 hash, f32 x, f32 y, f32 z) const {
    const i8* g = GRAD3[hash % 12];
    return (f32)g[0] * x + (f32)g[1] * y + (f32)g[2] * z;
}

f32 SimplexNoise::sample3D(f32 xin, f32 yin, f32 zin) const {
    // 1. Скос в решётку симплексов.
    const f32 s = (xin + yin + zin) * F3;
    const i32 i = fastFloor(xin + s);
    const i32 j = fastFloor(yin + s);
    const i32 k = fastFloor(zin + s);

    const f32 t = (f32)(i + j + k) * G3;
    const f32 x0 = xin - ((f32)i - t);
    const f32 y0 = yin - ((f32)j - t);
    const f32 z0 = zin - ((f32)k - t);

    // 2. Порядок обхода вершин симплекса определяется рангом координат.
    i32 i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0)      { i1=1; j1=0; k1=0; i2=1; j2=1; k2=0; }
        else if (x0 >= z0) { i1=1; j1=0; k1=0; i2=1; j2=0; k2=1; }
        else               { i1=0; j1=0; k1=1; i2=1; j2=0; k2=1; }
    } else {
        if (y0 < z0)       { i1=0; j1=0; k1=1; i2=0; j2=1; k2=1; }
        else if (x0 < z0)  { i1=0; j1=1; k1=0; i2=0; j2=1; k2=1; }
        else               { i1=0; j1=1; k1=0; i2=1; j2=1; k2=0; }
    }

    const f32 x1 = x0 - (f32)i1 + G3;
    const f32 y1 = y0 - (f32)j1 + G3;
    const f32 z1 = z0 - (f32)k1 + G3;
    const f32 x2 = x0 - (f32)i2 + 2.0f * G3;
    const f32 y2 = y0 - (f32)j2 + 2.0f * G3;
    const f32 z2 = z0 - (f32)k2 + 2.0f * G3;
    const f32 x3 = x0 - 1.0f + 3.0f * G3;
    const f32 y3 = y0 - 1.0f + 3.0f * G3;
    const f32 z3 = z0 - 1.0f + 3.0f * G3;

    // 3. Хэши углов.
    const i32 ii = i & 255, jj = j & 255, kk = k & 255;
    const i32 gi0 = perm_[ii      + perm_[jj      + perm_[kk     ]]] % 12;
    const i32 gi1 = perm_[ii + i1 + perm_[jj + j1 + perm_[kk + k1]]] % 12;
    const i32 gi2 = perm_[ii + i2 + perm_[jj + j2 + perm_[kk + k2]]] % 12;
    const i32 gi3 = perm_[ii + 1  + perm_[jj + 1  + perm_[kk + 1 ]]] % 12;

    // 4. Вклад каждого угла с радиальным затуханием.
    f32 n = 0.0f;
    f32 t0 = 0.6f - x0*x0 - y0*y0 - z0*z0;
    if (t0 > 0) { t0 *= t0; n += t0 * t0 * grad(gi0, x0, y0, z0); }
    f32 t1 = 0.6f - x1*x1 - y1*y1 - z1*z1;
    if (t1 > 0) { t1 *= t1; n += t1 * t1 * grad(gi1, x1, y1, z1); }
    f32 t2 = 0.6f - x2*x2 - y2*y2 - z2*z2;
    if (t2 > 0) { t2 *= t2; n += t2 * t2 * grad(gi2, x2, y2, z2); }
    f32 t3 = 0.6f - x3*x3 - y3*y3 - z3*z3;
    if (t3 > 0) { t3 *= t3; n += t3 * t3 * grad(gi3, x3, y3, z3); }

    // Множитель 32 приводит результат к диапазону примерно [-1, 1].
    return 32.0f * n;
}

f32 SimplexNoise::fbm3D(f32 x, f32 y, f32 z, u32 octaves,
                        f32 lacunarity, f32 gain) const
{
    if (octaves == 0) return 0.0f;
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (u32 o = 0; o < octaves; ++o) {
        sum  += amp * sample3D(x * freq, y * freq, z * freq);
        norm += amp;
        amp  *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

f32 SimplexNoise::ridged3D(f32 x, f32 y, f32 z, u32 octaves,
                           f32 lacunarity, f32 gain) const
{
    if (octaves == 0) return 0.0f;
    f32 sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (u32 o = 0; o < octaves; ++o) {
        const f32 n = 1.0f - std::fabs(sample3D(x * freq, y * freq, z * freq));
        sum  += amp * n * n;
        norm += amp;
        amp  *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

f32 SimplexNoise::warped3D(f32 x, f32 y, f32 z, f32 strength) const {
    // Смещаем точку выборки тремя независимыми срезами того же шума —
    // получаются «завихрения» на границах биомов.
    const f32 wx = sample3D(x + 17.3f, y + 4.1f,  z + 91.7f);
    const f32 wy = sample3D(x - 53.9f, y + 62.5f, z - 7.3f);
    const f32 wz = sample3D(x + 8.6f,  y - 31.2f, z + 44.8f);
    return sample3D(x + wx * strength, y + wy * strength, z + wz * strength);
}

} // namespace world
