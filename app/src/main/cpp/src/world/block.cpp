#include "block.h"
#include "../core/log.h"
#include <cstring>

namespace world {

BlockRegistry& blocks() {
    static BlockRegistry inst;
    static bool init = false;
    if (!init) {
        init = true;

        auto reg = [&](u16 id, const char* name,
                       bool solid, bool transparent, bool liquid,
                       bool emissive, u8 light,
                       u8 top, u8 side, u8 bottom,
                       f32 hardness)
        {
            BlockDef d{};
            d.name = name;
            d.isSolid = solid;
            d.isTransparent = transparent;
            d.isLiquid = liquid;
            d.isEmissive = emissive;
            d.emitsLight = light > 0;
            d.hasCollision = solid;
            d.lightLevel = light;
            d.atlasTop = top;
            d.atlasSide = side;
            d.atlasBottom = bottom;
            d.hardness = hardness;
            d.toolFlags = 0;
            inst.registerBlock(id, d);
        };

        // ID — те же, что в enum BlockId
        reg(AIR,      "Air",       false, true,  false, false, 0, 0,  0,  0, 0.0f);
        reg(STONE,    "Stone",     true,  false, false, false, 0, 1,  1,  1, 1.5f);
        reg(DIRT,     "Dirt",      true,  false, false, false, 0, 2,  2,  2, 0.6f);
        reg(GRASS,    "Grass",     true,  false, false, false, 0, 3,  3,  2, 0.6f);
        reg(SAND,     "Sand",      true,  false, false, false, 0, 4,  4,  4, 0.5f);
        reg(WATER,    "Water",     false, true,  true,  false, 0, 5,  5,  5, 100.f);
        reg(WOOD,     "Wood",      true,  false, false, false, 0, 6,  6,  6, 1.2f);
        reg(LEAVES,   "Leaves",    true,  true,  false, false, 0, 7,  7,  7, 0.3f);
        reg(SNOW,     "Snow",      true,  false, false, false, 0, 8,  8,  8, 0.4f);
        reg(ICE,      "Ice",       true,  true,  false, false, 0, 9,  9,  9, 0.8f);
        reg(LAVA,     "Lava",      false, true,  true,  true, 15, 10, 10, 10, 100.f);
        reg(IRON_ORE, "Iron Ore",  true,  false, false, false, 0, 11, 11, 11, 2.5f);
        reg(GOLD_ORE, "Gold Ore",  true,  false, false, false, 0, 12, 12, 12, 3.0f);
        reg(BEDROCK,  "Bedrock",   true,  false, false, false, 0, 13, 13, 13, -1.f);
    }
    return inst;
}

void BlockRegistry::registerBlock(u16 id, const BlockDef& def) {
    if (id >= MAX_BLOCKS) { LOGE("Block id %u > MAX_BLOCKS", id); return; }
    defs_[id] = def;
    if (id > maxId_) maxId_ = id;
}

const BlockDef& BlockRegistry::get(u16 id) const {
    if (id >= MAX_BLOCKS) return defs_[AIR];
    return defs_[id];
}

} // namespace world
