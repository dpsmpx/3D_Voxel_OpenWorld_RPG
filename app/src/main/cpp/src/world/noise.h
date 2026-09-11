#pragma once
#include "../core/types.h"
#include <glm/glm.hpp>
#include <cmath>

namespace world {

// ============================================================
// 3D Simplex Noise. Реализация Gustavson (public domain).
// Поддерживает seed и fBm.
// ============================================================
class SimplexNoise {
public:
    explicit SimplexNoise(u64 seed = 0);

    // Одиночный вызов, результат ~[-1,1]
    f32 sample3D(f32 x, f32 y, f32 z) const;
    f32 sample2D(f32 x, f32 y) const { return sample3D(x, y, 0.0f); }

    // fBm — сумма октав с персистентностью
    f32 fbm3D(f32 x, f32 y, f32 z, u32 octaves,
              f32 lacunarity = 2.0f, f32 gain = 0.5f) const;

    // Доменные искажения — "вихревое" смещение, даёт интересные биомы
    f32 warped3D(f32 x, f32 y, f32 z, f32 strength) const;

private:
    u8 perm_[512];
    static constexpr f32 F3 = 1.f/3.f;
    static constexpr f32 G3 = 1.f/6.f;
};

// ============================================================
// Многослойная генерация мира: высота, пещеры, температура,
// влажность, руды. Используется ChunkManager'ом.
// ============================================================
struct TerrainGenerator {
    SimplexNoise height;
    SimplexNoise caves;
    SimplexNoise biomeTemp;
    SimplexNoise biomeHumid;
    SimplexNoise warped;
    SimplexNoise ore;

    explicit TerrainGenerator(u64 seed);

    // Высота ландшафта в мировых координатах x,z
    i32 surfaceHeight(i32 x, i32 z) const;

    // Тип блока для верхнего слоя биома
    u16 surfaceBlock(f32 temp, f32 humid) const;

    // Есть ли пещера в точке
    bool isCave(i32 x, i32 y, i32 z) const;

    // Есть ли руда
    u16 oreAt(i32 x, i32 y, i32 z, u16 baseBlock) const;
};

} // namespace world