#pragma once
#include "../core/types.h"
#include <array>
#include <string>

namespace world {

// Свойства блока — компилируются один раз, читаются из рендера/физики
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
    u8  atlasTop;        // индекс тайла в атласе
    u8  atlasSide;
    u8  atlasBottom;
    f32 hardness;        // время добычи в секундах
    u32 toolFlags;       // какие инструменты эффективны
};

// Реестр блоков — заполняется на старте
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

// Глобальные ID — согласованы с реестром
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
