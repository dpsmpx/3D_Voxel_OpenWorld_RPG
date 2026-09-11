#include "terrain.h"
#include <cmath>
#include <algorithm>

namespace world {

TerrainGenerator::TerrainGenerator(u64 seed)
    : biome_(seed),
      heightBase_(seed ^ 0xA55A),
      cavesA_(seed ^ 0xC0DE),
      cavesB_(seed ^ 0xBEEF),
      seed_(seed)
{}

i32 TerrainGenerator::surfaceHeight(i32 x, i32 z) const {
    // Базовая высота от климата
    BiomeField::Sample clim = biome_.sample(x, z, 32);
    f32 h = 32.f + clim.heightMod;

    // Локальная детализация (мелкий рельеф)
    f32 detail = heightBase_.fbm3D((f32)x * 0.015f, 0.f, (f32)z * 0.015f, 3);
    h += detail * 4.f;

    // Скалы в горах
    if (clim.peaks > 0.3f && clim.erosion < 0.5f) {
        f32 ridge = 1.f - std::fabs(heightBase_.sample3D((f32)x * 0.03f, 0.f, (f32)z * 0.03f));
        h += ridge * clim.peaks * 12.f;
    }

    if (h < 1.f)  h = 1.f;
    if (h > 127.f) h = 127.f;
    return (i32)h;
}

BiomeId TerrainGenerator::biomeAt(i32 x, i32 z) const {
    i32 h = surfaceHeight(x, z);
    return biome_.sample(x, z, h).biome;
}

BiomeField::Sample TerrainGenerator::sampleClimate(i32 x, i32 z, i32 surfaceY) const {
    return biome_.sample(x, z, surfaceY);
}

bool TerrainGenerator::isCave(i32 x, i32 y, i32 z) const {
    // Ridged noise: 1 - |n|. Две поверхности → "червячные" туннели.
    // Туннель образуется там, где ОБА значения близки к 1.
    const f32 scale = 1.f / 48.f;
    f32 a = cavesA_.sample3D((f32)x * scale, (f32)y * scale * 1.6f, (f32)z * scale);
    f32 b = cavesB_.sample3D((f32)x * scale, (f32)y * scale * 1.6f, (f32)z * scale);
    f32 ridgeA = 1.f - std::fabs(a);
    f32 ridgeB = 1.f - std::fabs(b);

    // Обе поверхности в узком коридоре
    if (ridgeA > 0.90f && ridgeB > 0.90f) return true;

    // Большие залы — низкочастотный шум
    f32 hall = cavesA_.fbm3D((f32)x * 0.008f, (f32)y * 0.015f, (f32)z * 0.008f, 3);
    if (hall > 0.65f && y < 40) return true;

    return false;
}

u16 TerrainGenerator::oreAt(i32 x, i32 y, i32 z) const {
    // Руды через хэш-сетки. Идея: решаем принадлежность (x,y,z) к жиле
    // через хэш-ячейку 4×4×4. Внутри ячейки — шум для формы.

    constexpr i32 CELL = 4;
    i32 cx = x >> 2, cy = y >> 2, cz = z >> 2;

    // Детерминированный хэш
    auto hash3 = [](i32 a, i32 b, i32 c, u64 s) -> u32 {
        u64 h = (u64)(u32)a * 0x9E3779B97F4A7C15ULL;
        h ^= (u64)(u32)b * 0xC4CEB9FE1A85EC53ULL;
        h ^= (u64)(u32)c * 0xFF51AFD7ED558CCDULL;
        h ^= s;
        h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
        return (u32)h;
    };

    // Проверяем 3x3x3 ячейки-соседа — жила может заходить в текущую
    for (i32 ddy = -1; ddy <= 1; ++ddy) {
        for (i32 ddz = -1; ddz <= 1; ++ddz) {
            for (i32 ddx = -1; ddx <= 1; ++ddx) {
                i32 vcx = cx + ddx, vcy = cy + ddy, vcz = cz + ddz;
                u32 h = hash3(vcx, vcy, vcz, seed_ ^ 0x0AE5ULL);
                // 12% ячеек содержат жилу
                if ((h & 0xFF) > 30) continue;
                // Тип руды зависит от высоты
                u16 oreKind;
                if (vcy * CELL > 80) continue;   // нет руды на поверхности
                if (vcy * CELL > 50) {
                    oreKind = ((h >> 8) & 3) == 0 ? GOLD_ORE : IRON_ORE;
                } else {
                    oreKind = ((h >> 8) & 3) < 2 ? IRON_ORE : GOLD_ORE;
                }
                // Центр жилы внутри ячейки
                f32 ox = (f32)(h & 0xF) / 16.f;
                f32 oy = (f32)((h >> 8) & 0xF) / 16.f;
                f32 oz = (f32)((h >> 16) & 0xF) / 16.f;
                glm::vec3 center = {
                    (f32)(vcx * CELL) + ox * (f32)CELL,
                    (f32)(vcy * CELL) + oy * (f32)CELL,
                    (f32)(vcz * CELL) + oz * (f32)CELL,
                };
                glm::vec3 p = { (f32)x + 0.5f, (f32)y + 0.5f, (f32)z + 0.5f };
                glm::vec3 d = p - center;
                f32 dist2 = glm::dot(d, d);
                // Радиус жилы 1.5..2.5
                f32 r = 1.5f + (f32)((h >> 24) & 0xFF) / 255.f;
                if (dist2 < r * r) return oreKind;
            }
        }
    }
    return STONE;
}

} // namespace world