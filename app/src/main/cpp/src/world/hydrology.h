/**
 * @file hydrology.h
 * @brief Мир: гидрология — водосборы, накопление стока, реки, озёра.
 *
 * Реки строятся в три слоя, а не одним трассировщиком от истока:
 *
 *   1. Грубая сетка с шагом NODE блоков. По ней заливаются бессточные
 *      впадины (priority-flood) и у каждого узла появляется ровно
 *      одно направление стока. Это и есть граф водосборов: дерево,
 *      корни которого — море.
 *   2. По графу накапливается сток. Каждый узел добавляет свой дождь
 *      (влажность климата, наветренные склоны, высокогорье), и вода
 *      передаётся вниз. Ширина русла выводится из накопленного
 *      стока, а не из пройденного расстояния: после каждого слияния
 *      река честно становится шире.
 *   3. Каждое речное ребро трассируется подробно: сплайн по узлам,
 *      меандры на равнине, уровень воды, спускающийся вместе с
 *      рельефом, пороги и водопады, озёра во впадинах, дельты в
 *      устьях больших рек.
 *
 * Мир бесконечный, поэтому считается он плитками TILE_NODES узлов
 * с «фартуком» APRON_NODES вокруг. Ребро рисует только та плитка, в
 * ядре которой лежит его верхний узел. Свойства узла (сток, уровень,
 * положение) не зависят от того, какое окно его посчитало, — пока
 * его водосбор и путь до моря помещаются в фартук. Замер по сотням
 * тысяч речных узлов: 99.9 % водосборов укладываются в 2000 блоков,
 * поэтому граница плитки для реки — граница кэша, а не гидрологии.
 *
 * Всё посчитанное живёт в кэше и дальше только читается: генерация
 * чанка берёт из плитки лишь сегменты, задевающие этот чанк.
 */
#pragma once
#include "../core/types.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace world {

class TerrainGenerator;

/// Упакованное течение верхней грани воды — тот же формат, что читает
/// voxel.frag: биты 0..2 — румб (0 = +X, 2 = +Z), 3..5 — скорость,
/// 6..7 — бурление.
u8 packWaterFlow(f32 dirX, f32 dirZ, u32 speedQ, u32 turbulence);

namespace hydro {

constexpr i32 NODE        = 32;    ///< шаг грубой сетки, блоков
constexpr i32 NODE_SHIFT  = 5;
constexpr i32 HT_NODES    = 32;    ///< плитка высот: 32x32 узла
constexpr i32 TILE_NODES  = 64;    ///< ядро гидро-плитки: 2048 блоков
constexpr i32 APRON_NODES = 64;    ///< фартук вокруг ядра: 2048 блоков
constexpr i32 WIN_NODES   = TILE_NODES + 2 * APRON_NODES;
constexpr i32 TILE_BLOCKS = TILE_NODES * NODE;

/// Сетка плиток сдвинута на полплитки: начало координат лежит в
/// середине плитки (0, 0). Мир у точки появления строится из одной
/// плитки, а не из четырёх, сходящихся там углами.
constexpr i32 TILE_OFFSET_NODES = TILE_NODES / 2;

/// Первый узел ядра плитки t по одной оси.
constexpr i32 tileFirstNode(i32 t) { return t * TILE_NODES - TILE_OFFSET_NODES; }
/// Плитка, в ядре которой лежит узел n.
i32 tileOfNode(i32 n);
/// Плитка, в ядре которой лежит блок b.
i32 tileOfBlock(i32 b);

/// Насколько далеко за ядро плитки дотягивается влияние её рек:
/// ребро длиной до полутора узлов, меандр, долина и рукава дельты.
constexpr i32 MAX_REACH   = 160;

/// Накопленный сток, с которого начинается русло. Единица — дождь
/// на узел сетки в умеренном климате.
constexpr f32 RIVER_FLOW  = 18.f;

/// Полуширина русла по стоку: ширина растёт как корень из расхода.
f32 halfWidthForFlow(f32 flow);

enum SampleFlags : u8 {
    SF_STONE_BED = 1 << 0,   ///< горный поток: дно — камень
    SF_SAND_BANK = 1 << 1,   ///< песчаные берега большой равнинной реки
    SF_DELTA     = 1 << 2,   ///< рукав дельты: низкий песчаный берег
    SF_IN_LAKE   = 1 << 3,   ///< ребро внутри озера: долину не режем
    SF_PLUNGE    = 1 << 4,   ///< под водопадом: котёл глубже
    SF_FROZEN    = 1 << 5,   ///< тундра: вода подо льдом
};

/// Точка осевой линии русла.
struct Sample {
    f32 x = 0.f, z = 0.f;
    f32 u = 0.f;       ///< координата вдоль реки (расстояние до устья)
    f32 hw = 1.f;      ///< полуширина русла
    f32 depth = 1.f;   ///< глубина по оси
    f32 bank = 0.f;    ///< ровная пойма за урезом воды
    f32 wall = 1.f;    ///< подъём склона долины на блок
    f32 reach = 4.f;   ///< радиус влияния: край долины
    f32 braid = 0.f;   ///< 0..1 — насколько русло разбито на протоки
    i16 waterY = 0;    ///< верхний блок воды
    u8  flow = 0;      ///< packWaterFlow
    u8  flags = 0;     ///< SampleFlags
};

enum PathKind : u8 { PATH_RIVER = 0, PATH_DELTA = 1 };

/// Одно ребро графа (или рукав дельты) — непрерывный отрезок проб.
struct Path {
    u32  first = 0, count = 0;
    i32  fromI = 0, fromJ = 0;   ///< узел, из которого течёт
    i32  toI = 0, toJ = 0;       ///< узел, в который впадает
    f32  flow = 0.f;             ///< накопленный сток на ребре
    u32  basin = 0;              ///< стабильный номер водосбора
    u8   order = 1;              ///< порядок по Стралеру
    u8   kind = PATH_RIVER;
    bool mainStem = false;       ///< продолжает главный ствол приёмника
    bool mouth = false;          ///< приёмник — море
};

/// Отрезок между соседними пробами с рамкой влияния.
struct Segment {
    u32 a = 0;                   ///< проба-начало; конец — a + 1
    f32 x0 = 0.f, z0 = 0.f, x1 = 0.f, z1 = 0.f;
};

enum NodeFlags : u8 {
    NF_SEA       = 1 << 0,
    NF_RIVER     = 1 << 1,
    NF_LAKE      = 1 << 2,
    NF_UNDRAINED = 1 << 3,       ///< путь к морю не уместился в окно
    NF_FROZEN    = 1 << 4,
};

/// Посчитанная плитка. Неизменяема: её держат shared_ptr, и чанк,
/// взявший её, спокойно дочитывает даже вытесненную из кэша.
struct Tile {
    i32 tx = 0, tz = 0;
    std::vector<Sample>  samples;
    std::vector<Path>    paths;
    std::vector<Segment> segs;
    /// (ключ чанка, номер сегмента), отсортировано по ключу. Ключ —
    /// чанк относительно угла плитки, по 16 бит на ось: вдвое меньше
    /// памяти, чем мировые координаты.
    std::vector<std::pair<u32, u32>> chunkSegs;

    // Узлы ядра, индекс j * TILE_NODES + i.
    std::vector<u8>  nodeFlags;
    std::vector<i16> lakeLevel;  ///< верхний блок озера, -1 — не озеро
    std::vector<i16> nodeLevel;  ///< уровень реки в узле, -1 — нет
    std::vector<f32> nodeFlow;
    std::vector<u32> nodeBasin;
    std::vector<u8>  nodeOrder;
};

/// Узел грубой сетки глазами снаружи: проверки, карта, геймплей.
struct NodeInfo {
    u8  flags = 0;
    i16 lakeLevel = -1;
    i16 level = -1;
    f32 flow = 0.f;
    u32 basin = 0;
    u8  order = 0;
};

/// Всё, что генерации одного чанка нужно от гидрологии.
struct ChunkView {
    i32 cx = 0, cz = 0;
    std::vector<std::shared_ptr<const Tile>> tiles;   ///< держат память проб
    struct Seg {
        const Sample* a;
        const Sample* b;
        f32 x0, z0, x1, z1;
    };
    std::vector<Seg> segs;
    /// Уровни озёр в четырёх узлах вокруг чанка: (cx,cz), (cx+1,cz),
    /// (cx,cz+1), (cx+1,cz+1). -1 — не озеро.
    i16 lake[4] = { -1, -1, -1, -1 };
    bool lakeFrozen[4] = { false, false, false, false };

    bool empty() const {
        return segs.empty() &&
               lake[0] < 0 && lake[1] < 0 && lake[2] < 0 && lake[3] < 0;
    }
};

/// Что реки сделали с одной колонкой.
struct ColumnWater {
    enum Kind : u8 { None = 0, Ground, Channel, Lake };
    Kind kind = None;
    i32  surface = 0;       ///< первый не-твёрдый блок после рек
    i32  waterTop = -1;     ///< верхний блок воды, -1 — воды нет
    u8   flow = 0;          ///< запечённое течение верхней грани
    u8   flags = 0;         ///< SampleFlags ближайшего русла
    bool bar = false;       ///< коса между протоками
    bool bank = false;      ///< прибрежная полоса у самой воды
    bool frozen = false;
};

class Hydrology {
public:
    Hydrology(const TerrainGenerator& terrain, u64 seed);
    ~Hydrology();
    Hydrology(const Hydrology&) = delete;
    Hydrology& operator=(const Hydrology&) = delete;

    /// Плитка (tx, tz): ядро [tx*TILE_BLOCKS, (tx+1)*TILE_BLOCKS).
    /// Считается один раз; конкурентные запросы ждут первого.
    std::shared_ptr<const Tile> tile(i32 tx, i32 tz) const;

    /// Сегменты и озёра, задевающие чанк.
    void chunkView(i32 cx, i32 cz, ChunkView& out) const;

    /// Колонка после рек. raw — высота поверхности по генератору.
    ///
    /// segs/count — подмножество v.segs, заведомо содержащее все
    /// сегменты, чья рамка накрывает колонку (генерация чанка отбирает
    /// их по блокам 8x8). По умолчанию — все сегменты вида. Ответ от
    /// выбора не зависит: отброшенные сегменты колонку не задевают.
    static ColumnWater evaluate(const ChunkView& v, i32 wx, i32 wz, i32 raw,
                                const ChunkView::Seg* segs = nullptr,
                                usize count = 0);

    /// Точечный запрос: то же, что даст генерация чанка для этой
    /// колонки. Нужен деревьям соседних чанков, спавну и проверкам.
    ColumnWater column(i32 wx, i32 wz, i32 raw) const;

    NodeInfo node(i32 i, i32 j) const;

    /// Готова ли плитка — без ожидания и без расчёта. Нужно тому, кто
    /// заказывает плитки заранее, в фоне.
    bool tileReady(i32 tx, i32 tz) const;

    /// Сколько плиток посчитано за жизнь объекта (для проверок кэша).
    u32 tilesBuilt() const { return tilesBuilt_.load(std::memory_order_relaxed); }

    u32 id() const { return id_; }

private:
    struct HeightTile;
    struct HeightSlot;
    struct TileSlot;

    std::shared_ptr<const HeightTile> heightTile(i32 hx, i32 hz) const;
    std::shared_ptr<Tile> build(i32 tx, i32 tz) const;

    const TerrainGenerator& terrain_;
    u32 id_ = 0;
    i32 windDx_ = 1, windDz_ = 0;

    mutable std::mutex heightMtx_;
    mutable std::unordered_map<u64, std::shared_ptr<HeightSlot>> heights_;
    mutable u64 heightClock_ = 0;

    mutable std::mutex tileMtx_;
    mutable std::unordered_map<u64, std::shared_ptr<TileSlot>> tiles_;
    mutable u64 tileClock_ = 0;

    mutable std::atomic<u32> tilesBuilt_{0};
};

} // namespace hydro
} // namespace world
