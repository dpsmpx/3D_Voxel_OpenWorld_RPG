#pragma once
#include "biome.h"
#include "noise.h"
#include "../core/types.h"
#include <glm/glm.hpp>

namespace world {

// ============================================================
// TerrainGenerator — многослойная генерация высоты и климата.
// Заменяет Phase 1 TerrainGenerator: добавлены континенты,
// эрозия, пики, пещеры через ridged-noise, рудные жилы.
// ============================================================
class TerrainGenerator {
public:
    explicit TerrainGenerator(u64 seed);

    // Высота поверхности (мировые координаты).
    i32 surfaceHeight(i32 x, i32 z) const;

    // Биом в точке.
    BiomeId biomeAt(i32 x, i32 z) const;

    // Готовый сэмпл климата (кэшируется вызывающим).
    BiomeField::Sample sampleClimate(i32 x, i32 z, i32 surfaceY) const;

    // Пещеры: ridged-3D, две поверхности → связанные туннели.
    bool isCave(i32 x, i32 y, i32 z) const;

    // Руды: жилы, а не единичные блоки. Детерминированы по (chunk, veinIdx).
    // Возвращает ID руды или STONE.
    u16 oreAt(i32 x, i32 y, i32 z) const;

    // Уровень моря (для воды)
    static constexpr i32 SEA_LEVEL = 24;

    // Доступ к климату и шуму для features
    const BiomeField& field() const { return biome_; }
    const SimplexNoise& noise() const { return caves_; }

private:
    mutable BiomeField biome_;
    SimplexNoise heightBase_;
    SimplexNoise cavesA_;
    SimplexNoise cavesB_;
    u64 seed_;
};

} // namespace world