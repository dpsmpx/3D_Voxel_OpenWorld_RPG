/**
 * @file terrain.h
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#pragma once
#include "biome.h"
#include "noise.h"
#include "../core/types.h"
#include <glm/glm.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace world {

/// TerrainGenerator — многослойная генерация высоты и климата.
/// Заменяет Phase 1 TerrainGenerator: добавлены континенты,
/// эрозия, пики, пещеры через ridged-noise, рудные жилы.
class TerrainGenerator {
public:
    explicit TerrainGenerator(u64 seed);

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

    /// Руды: жилы, а не единичные блоки. Детерминированы по (chunk, veinIdx).
    /// Возвращает ID руды или STONE.
    u16 oreAt(i32 x, i32 y, i32 z) const;

    /// Уровень моря (для воды)
    static constexpr i32 SEA_LEVEL = 24;

    // ------------------------------------------------------------
    // Реки
    //
    // Реки процедурны, но не рисуются как полосы фиксированного уровня.
    // Сначала строится детерминированная сеть: источник в высокогорье,
    // русло спускается по полю рельефа к побережью, а затем от главного
    // русла проводятся более короткие притоки. Воксельный генератор
    // использует эту сеть только для пересекающегося чанка.
    // ------------------------------------------------------------
    static constexpr i32 RIVER_CELL_SIZE = 1024;
    static constexpr i32 RIVER_MAX_LENGTH = 4096;

    struct RiverPoint {
        i32 x = 0;
        i32 z = 0;
        i16 terrainY = 0;
        i16 waterY = 0;   ///< высота верхнего водного блока
        f32 width = 0.f; ///< радиус русла в блоках
    };

    struct RiverPath {
        std::vector<RiverPoint> points;
        bool tributary = false;

        RiverPoint& front() { return points.front(); }
        const RiverPoint& front() const { return points.front(); }
        RiverPoint& back() { return points.back(); }
        const RiverPoint& back() const { return points.back(); }
    };

    struct RiverNetwork {
        glm::ivec3 source{0};
        std::vector<RiverPath> paths;
    };

    /// Детерминированная сеть для ячейки 1024x1024 блоков.
    /// Пустой результат означает, что подходящего высокогорного
    /// истока в этой ячейке нет.
    std::shared_ptr<const RiverNetwork> riverNetworkAtCell(i32 cellX,
                                                             i32 cellZ) const;

    /// Все сети, которые потенциально могут пересечь указанный чанк.
    /// Результат владеет объектами через shared_ptr, поэтому кэш можно
    /// безопасно ограничивать без висячих ссылок.
    void riverNetworksNear(i32 chunkX, i32 chunkZ,
                           std::vector<std::shared_ptr<const RiverNetwork>>& out) const;

    /// Seed мира — нужен процедурным features без дублирования состояния.
    u64 seed() const { return seed_; }

    /// Доступ к климату и шуму для features
    const BiomeField& field() const { return biome_; }
    const SimplexNoise& noise() const { return cavesA_; }

private:
    mutable BiomeField biome_;
    SimplexNoise heightBase_;
    SimplexNoise cavesA_;
    SimplexNoise cavesB_;
    u64 seed_;

    // Кэш детерминированных river networks. Он разделяется всеми
    // рабочими потоками генерации чанков; копии одной и той же сети
    // поэтому не строятся заново для каждого чанка.
    mutable std::mutex riverCacheMtx_;
    mutable std::unordered_map<u64, std::shared_ptr<const RiverNetwork>> riverCache_;
};

} // namespace world
