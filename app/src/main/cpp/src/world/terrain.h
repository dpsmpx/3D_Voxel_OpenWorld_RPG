/**
 * @file terrain.h
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#pragma once
#include "biome.h"
#include "hydrology.h"
#include "landform.h"
#include "noise.h"
#include "../core/types.h"
#include <glm/glm.hpp>
#include <memory>

namespace world {

/// TerrainGenerator — многослойная генерация высоты и климата.
/// Заменяет Phase 1 TerrainGenerator: добавлены континенты,
/// эрозия, пики, пещеры через ridged-noise, рудные жилы.
class TerrainGenerator {
public:
    explicit TerrainGenerator(u64 seed);
    ~TerrainGenerator();
    TerrainGenerator(const TerrainGenerator&) = delete;
    TerrainGenerator& operator=(const TerrainGenerator&) = delete;

    /// Колонка мира: высота поверхности вместе с климатом.
    /// Считать их раздельно накладно — высота выводится из тех же
    /// пяти шумовых полей, что и биом, поэтому расчёт общий.
    struct Column {
        i32                surface;   ///< высота поверхности
        BiomeField::Sample climate;   ///< поля климата и выбранный биом
        /// До какой высоты колонка залита лавой; 0 — не залита.
        ///
        /// Кратер вулкана. Обычные жидкости наливает applyLiquids от
        /// поверхности до уровня моря, а вулкан стоит много выше
        /// моря — по тому правилу лавы в нём не было бы ни капли, и
        /// вулкан отличался бы от горы только цветом камня.
        i32                lavaTop = 0;
        /// Высота до округления. Гидрологии нужна она: у целых высот
        /// равнина состоит из плоских ступеней, и сток на них не
        /// знает, куда течь.
        f32                heightF = 0.f;
        /// Высота для стока (LandformSample::route): крупный рельеф
        /// без холмов. Её, а не heightF, читает гидросеть.
        f32                heightRoute = 0.f;
        /// Характер местности (landform.h): равнина, холмы, плато, горы.
        Landform           landform = Landform::Plains;
        /// 0..1: близость к гребню хребта — где выходит скала.
        f32                ridge = 0.f;
    };

    /// Выше этой высоты горы лежат под снегом.
    ///
    /// Не украшение: голый камень от подножия до вершины читается
    /// как карьер, а не как гора. Снеговая линия — то, по чему
    /// вершину узнают издали.
    static constexpr i32 SNOW_LINE = 84;
    Column column(i32 x, i32 z) const;

    /// Высота поверхности (мировые координаты).
    /// Внутри вызывает column(); если нужен ещё и климат — берите column().
    i32 surfaceHeight(i32 x, i32 z) const;

    /// Биом в точке.
    BiomeId biomeAt(i32 x, i32 z) const;

    // Готовый сэмпл климата (кэшируется вызывающим).
    BiomeField::Sample sampleClimate(i32 x, i32 z, i32 surfaceY) const;

    /// Пещеры. Поле плотности считается отдельно от порога, чтобы
    /// генератор мог сэмплировать его на разреженной решётке и
    /// интерполировать: проверка каждого вокселя напрямую стоила
    /// пяти выборок шума на воксель.
    struct CaveDensity {
        f32 tunnel;   ///< min двух ridged-поверхностей: туннели
        f32 hall;     ///< низкочастотный шум: большие залы
    };
    CaveDensity caveDensity(f32 x, f32 y, f32 z) const;

    /// Порог по плотности. y нужен: залы бывают только в глубине.
    static bool isCaveAt(const CaveDensity& d, i32 y) {
        if (d.tunnel > 0.90f) return true;
        return d.hall > 0.65f && y < 40;
    }

    /// Точечная проверка (физика, спавн). В массовой генерации
    /// используйте caveDensity() с решёткой.
    bool isCave(i32 x, i32 y, i32 z) const;

    /// Руды: жилы, а не единичные блоки. Жила — шар в своей клетке
    /// 4×4×4; есть она в клетке или нет — по хэшу клетки.
    struct OreVein {
        glm::vec3 center;
        f32       radius;
        u16       kind;
    };
    static constexpr i32 ORE_CELL = 4;
    /// Жила клетки (cx, cy, cz), если она там есть. Выше 84-го блока
    /// жил нет вовсе.
    bool oreVeinAt(i32 cx, i32 cy, i32 cz, OreVein& out) const;
    /// Руда в точке или STONE. Жилы соседних клеток перекрываются;
    /// побеждает клетка, меньшая по (y, z, x). Массовая генерация
    /// обходит жилы, а не воксели (applyOres), с тем же правилом.
    u16 oreAt(i32 x, i32 y, i32 z) const;

    /// Уровень моря (для воды)
    static constexpr i32 SEA_LEVEL = 24;

    // ------------------------------------------------------------
    // Реки и озёра
    //
    // Не набор независимых трасс «исток → море», а одна гидросеть:
    // грубая сетка водосборов с заливкой впадин, накопление стока по
    // графу, из которого выводятся ширина русел, притоки всех
    // порядков, озёра и дельты. Подробности — в hydrology.h.
    // ------------------------------------------------------------
    const hydro::Hydrology& hydrology() const { return *hydro_; }

    /// Seed мира — нужен процедурным features без дублирования состояния.
    u64 seed() const { return seed_; }

    /// Доступ к климату и шуму для features
    const BiomeField& field() const { return biome_; }
    const SimplexNoise& noise() const { return cavesA_; }
    /// Шум растительности: рощи, поляны, цветочные луга (world/flora.h).
    const SimplexNoise& floraNoise() const { return flora_; }

private:
    mutable BiomeField biome_;
    LandformNoise landform_;
    SimplexNoise cavesA_;
    SimplexNoise cavesB_;
    SimplexNoise flora_;
    u64 seed_;

    // Гидросеть: кэш плиток водосборов, общий для всех потоков
    // генерации. Объявлена после seed_: строится из него.
    std::unique_ptr<hydro::Hydrology> hydro_;
};

} // namespace world
