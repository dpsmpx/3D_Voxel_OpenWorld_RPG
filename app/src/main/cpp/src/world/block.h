/**
 * @file block.h
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#pragma once
#include "../core/types.h"
#include <array>
#include <string>

namespace world {

/// Цвет грани в формате 0xRRGGBBAA.
///
/// Мир воксельный: у блока нет текстуры, у него есть материал. Цвет —
/// его единственное графическое свойство, остальное делают геометрия,
/// освещение и затенение углов.
///
/// Три цвета вместо одного — НЕ способ затенить грань. Затенением
/// занимается ровно одно место, таблица FACE_LIGHT в
/// shaders/voxel.frag, и оно применяется поверх любого из этих
/// цветов. Здесь разные цвета ставятся только там, где грани и правда
/// из разного материала: зелёная макушка на земляном боку у травы,
/// светлый спил на тёмной коре у дерева. У всего остального три поля
/// совпадают.
///
/// Раньше совпадали не все: у камня, песка, снега и руд верх был
/// светлее бока, а бок светлее низа. Наклон получался двойной — этот
/// плюс полусферный свет в шейдере, — и подправить его было нельзя,
/// не разбираясь каждый раз, какая из двух половин сейчас видна.
using BlockColor = u32;

constexpr BlockColor bcolor(u8 r, u8 g, u8 b, u8 a = 255) {
    return ((BlockColor)r << 24) | ((BlockColor)g << 16) |
           ((BlockColor)b << 8)  |  (BlockColor)a;
}

/// Свойства блока — компилируются один раз, читаются из рендера/физики
struct BlockDef {
    const char* name;
    u8  isSolid       : 1;
    u8  isTransparent : 1;
    u8  isLiquid      : 1;
    u8  isEmissive    : 1;
    u8  emitsLight    : 1;
    u8  hasCollision  : 1;
    u8  _pad          : 2;
    u8  lightLevel;      // 0..15 если emissive
    BlockColor colorTop;
    BlockColor colorSide;
    BlockColor colorBottom;
    f32 hardness;        // время добычи в секундах
    u32 toolFlags;       // какие инструменты эффективны

    /// Цвет грани по её индексу: 0..5 это +X, -X, +Y, -Y, +Z, -Z.
    BlockColor faceColor(u8 face) const {
        return face == 2 ? colorTop : (face == 3 ? colorBottom : colorSide);
    }
};

/// Реестр блоков — заполняется на старте
class BlockRegistry {
public:
    static constexpr u16 MAX_BLOCKS = 1024;

    void registerBlock(u16 id, const BlockDef& def);
    const BlockDef& get(u16 id) const;

    bool isSolid(u16 id)       const { return get(id).isSolid; }
    bool isTransparent(u16 id) const { return get(id).isTransparent; }
    bool isOpaque(u16 id)      const { return !get(id).isTransparent; }

private:
    std::array<BlockDef, MAX_BLOCKS> defs_{};
    u16 maxId_ = 0;
};

/// Глобальные ID — согласованы с реестром
enum BlockId : u16 {
    AIR     = 0,
    STONE   = 1,
    DIRT    = 2,
    GRASS   = 3,
    SAND    = 4,
    WATER   = 5,
    WOOD    = 6,
    LEAVES  = 7,
    SNOW    = 8,
    ICE     = 9,
    LAVA    = 10,
    IRON_ORE= 11,
    GOLD_ORE= 12,
    BEDROCK = 13,

    /// Количество заданных ID. Служит верхней границей для таблиц,
    /// индексируемых блоком (атлас текстур, block→item).
    BLOCK_COUNT
};

/// «Неизвестно»: соседний чанк ещё не загружен.
///
/// Это НЕ блок и НЕ воздух. Раньше отсутствующий сосед отдавался как
/// AIR, и мешер честно строил по такой границе наружную грань — стену
/// во всю толщу земли. Пока сосед не подгрузится, она так и стоит
/// перед камерой чёрным клином: у грани, смотрящей внутрь массива,
/// открытость неба нулевая.
///
/// Значение вне диапазона реестра намеренно: любое обращение
/// blocks().get(UNKNOWN) — ошибка, и её видно сразу.
constexpr u16 UNKNOWN = 0xFFFF;

/// Можно ли решать по этому соседу, строить грань или нет.
inline bool neighborKnown(u16 id) { return id != UNKNOWN; }

/// Рисуется ли материал со смешиванием, то есть видно ли сквозь него.
/// Смотрим на альфу цвета, а не на флаг isTransparent: листва и лава
/// помечены прозрачными, чтобы мешер не склеивал их грани, но рисуются
/// они непрозрачными. Сквозь видно только воду и лёд.
inline bool isSeeThrough(const BlockDef& d) {
    return (d.colorTop    & 0xFFu) < 255u
        || (d.colorSide   & 0xFFu) < 255u
        || (d.colorBottom & 0xFFu) < 255u;
}

BlockRegistry& blocks();  // синглтон

} // namespace world
