#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>
#include <cmath>

namespace world {

// ============================================================
// 3D Simplex Noise (алгоритм Gustavson, public domain).
// Детерминирован по seed: одна и та же пара (seed, точка) всегда
// даёт один результат — на этом держится воспроизводимость мира.
//
// Многослойная генерация (высота, климат, пещеры, руды) живёт в
// TerrainGenerator, см. terrain.h.
// ============================================================
class SimplexNoise {
public:
    explicit SimplexNoise(u64 seed = 0);

    /// Одиночная выборка, результат примерно в [-1, 1].
    f32 sample3D(f32 x, f32 y, f32 z) const;

    /// Двумерный срез трёхмерного шума.
    f32 sample2D(f32 x, f32 y) const { return sample3D(x, y, 0.0f); }

    /// fBm — сумма октав с затуханием амплитуды, нормирована к [-1, 1].
    f32 fbm3D(f32 x, f32 y, f32 z, u32 octaves,
              f32 lacunarity = 2.0f, f32 gain = 0.5f) const;

    /// Ridged-шум: 1 - |noise|, даёт хребты и связные туннели.
    f32 ridged3D(f32 x, f32 y, f32 z, u32 octaves,
                 f32 lacunarity = 2.0f, f32 gain = 0.5f) const;

    /// Доменное искажение: смещает точку выборки другим шумом.
    f32 warped3D(f32 x, f32 y, f32 z, f32 strength) const;

private:
    f32 grad(i32 hash, f32 x, f32 y, f32 z) const;

    u8 perm_[512];
    static constexpr f32 F3 = 1.0f / 3.0f;
    static constexpr f32 G3 = 1.0f / 6.0f;
};

} // namespace world
