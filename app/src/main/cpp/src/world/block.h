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
/// освещение и затенение углов. Разные цвета у верха, боков и низа
/// нужны не для «рисунка», а чтобы подчеркнуть форму: у травы зелёная
/// макушка на земляном боку, у дерева спил светлее коры.
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
    /// Подкрашивается ли материал по местности. Трава и листва меняют
    /// оттенок от опушки к опушке — без этого поляна на весь горизонт
    /// выглядит выкрашенной одной банкой. Камню и песку это не нужно.
    u8  biomeTint     : 1;
    u8  _pad          : 1;
    u8  lightLevel;      // 0..15 если emissive
    /// Насколько заметна поверхностная крапчатость, 0..7. Ровный
    /// цвет без единого текселя выглядит пластиком, поэтому шейдер
    /// подмешивает шум с шагом в один воксель — и его размах задаётся
    /// здесь: камню много, снегу почти ничего. Диапазон ровно тот,
    /// что влезает в три бита вершины: шире его всё равно не донести.
    u8  grain;
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

BlockRegistry& blocks();  // синглтон

} // namespace world
