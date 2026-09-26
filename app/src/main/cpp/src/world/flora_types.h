/**
 * @file flora_types.h
 * @brief Мир: растения и мелкие природные вещи — что это и где стоит.
 *
 * Рельеф собран из крупных блоков, а всё, что на нём растёт и лежит,
 * — из мелких вокселей, в разы мельче блока: дерево, куст, пучок
 * травы, цветы, камешки, коряга. Именно разница масштабов и даёт миру
 * богатую поверхность при крупноблочной земле.
 *
 * Растение в чанке — не блоки, а короткая запись: вид, вариант,
 * поворот, размер и место. Рисует его рендер экземплярами общих
 * моделей (render/flora_models.h). У деревьев и кактусов есть ствол —
 * невидимые блоки TRUNK / CACTUS_CORE в сетке мира: на них держится
 * всё, что знает о блоках, — столкновения, поиск пути, рубка, огонь,
 * сохранение правок.
 */
#pragma once
#include "../core/types.h"

namespace world {

enum class FloraKind : u8 {
    // ---- деревья: у них ствол (невидимые блоки) ----
    Oak = 0,
    Birch,
    Pine,
    Acacia,
    Palm,
    DeadTree,
    Cactus,
    // ---- подлесок и кустарник ----
    Bush,
    DryBush,      ///< сухой куст: степь, пустыня
    Fern,
    // ---- трава и цветы ----
    Grass,        ///< пучок травы
    TallGrass,
    Flowers,      ///< цвет — вариант
    Reeds,        ///< тростник у воды
    // ---- камни и мелочь ----
    Pebbles,
    Rock,         ///< валун меньше блока
    Log,          ///< упавший ствол
    Stump,
    Mushroom,
    Count
};

/// Сколько вариантов у каждого вида (модели разные, не только цвет).
constexpr u8 FLORA_VARIANTS[(u32)FloraKind::Count] = {
    3, 3, 3, 2, 2, 2, 3,   // деревья
    3, 2, 2,               // подлесок
    3, 2, 4, 2,            // трава, цветы (4 цвета), тростник
    2, 3, 2, 1, 2,         // камни, коряга, пень, грибы
};

/// Класс по дальности отрисовки: мелочь видна вблизи, деревья — до
/// горизонта. Порядок классов — порядок записей в списке чанка.
enum class FloraClass : u8 { Tree = 0, Medium, Small, Count };

inline FloraClass floraClass(FloraKind k) {
    if (k <= FloraKind::Cactus) return FloraClass::Tree;
    if (k <= FloraKind::Fern || k == FloraKind::Rock || k == FloraKind::Log ||
        k == FloraKind::Stump || k == FloraKind::Reeds) return FloraClass::Medium;
    return FloraClass::Small;
}

/// Есть ли у вида ствол в сетке мира (рубится, держит столкновение).
inline bool floraIsTree(FloraKind k) { return k <= FloraKind::Cactus; }

/// Запись о растении в чанке. 10 байт: у густого леса их сотни на
/// чанк, и память важнее удобства.
struct FloraInstance {
    u8        lx = 0, lz = 0;   ///< колонка в чанке
    i16       y = 0;            ///< первый воздух над землёй: здесь стоит
    FloraKind kind = FloraKind::Grass;
    u8        variant = 0;
    u8        yaw = 0;          ///< поворот, 0..255 → 0..2π
    u8        scale = 128;      ///< 0..255 → 0.5..1.3
    u8        offset = 0x88;    ///< сдвиг внутри блока: младшие 4 бита — x, старшие — z
    u8        trunk = 0;        ///< высота ствола в блоках (деревья), иначе 0
};
static_assert(sizeof(FloraInstance) == 10, "запись растения — 10 байт");

inline f32 floraScale(const FloraInstance& f) { return 0.5f + (f32)f.scale * (0.8f / 255.f); }
inline f32 floraYaw(const FloraInstance& f)   { return (f32)f.yaw * (6.2831853f / 256.f); }
/// Сдвиг от середины блока, в блоках: ±0.4.
inline f32 floraOffX(const FloraInstance& f)  { return ((f32)(f.offset & 15) - 7.5f) * (0.8f / 15.f); }
inline f32 floraOffZ(const FloraInstance& f)  { return ((f32)(f.offset >> 4) - 7.5f) * (0.8f / 15.f); }

} // namespace world
