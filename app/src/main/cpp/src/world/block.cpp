/**
 * @file block.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
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
                       BlockColor top, BlockColor side, BlockColor bottom,
                       u8 grain, f32 hardness)
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
            d.grain = grain;
            d.biomeTint = (id == GRASS || id == LEAVES);
            d.colorTop = top;
            d.colorSide = side;
            d.colorBottom = bottom;
            d.hardness = hardness;
            d.toolFlags = 0;
            inst.registerBlock(id, d);
        };

        // Палитра мира. Подбиралась по трём правилам: соседние
        // материалы должны различаться по светлоте, а не только по
        // тону (иначе в тени они сливаются); ни один цвет не выкручен
        // в чистый спектр, чтобы освещение оставалось читаемым; у
        // блоков с выраженным верхом он светлее бока на четверть —
        // так грань видна даже при свете точно сверху.
        //
        //  ID          имя        тв.  прз.  жид.  свеч. св.  верх                    бок                     низ                     зерно  прочн.
        reg(AIR,      "Air",       false, true,  false, false, 0, bcolor(  0,  0,  0,0), bcolor(  0,  0,  0,0), bcolor(  0,  0,  0,0),  0,   0.0f);
        reg(STONE,    "Stone",     true,  false, false, false, 0, bcolor(148,150,158),   bcolor(134,136,145),   bcolor(124,126,134),   4,   1.5f);
        reg(DIRT,     "Dirt",      true,  false, false, false, 0, bcolor(126, 92, 58),   bcolor(118, 86, 54),   bcolor(104, 76, 47),   4,   0.6f);
        reg(GRASS,    "Grass",     true,  false, false, false, 0, bcolor(124,186, 84),   bcolor(118, 86, 54),   bcolor(104, 76, 47),   3,   0.6f);
        reg(SAND,     "Sand",      true,  false, false, false, 0, bcolor(226,208,150),   bcolor(214,196,140),   bcolor(200,183,130),   2,   0.5f);
        reg(WATER,    "Water",     false, true,  true,  false, 0, bcolor( 62,124,196,150), bcolor( 54,110,180,170), bcolor( 54,110,180,170), 1, 100.f);
        reg(WOOD,     "Wood",      true,  false, false, false, 0, bcolor(158,122, 74),   bcolor(104, 74, 44),   bcolor(158,122, 74),   4,   1.2f);
        reg(LEAVES,   "Leaves",    true,  true,  false, false, 0, bcolor( 92,152, 66),   bcolor( 78,134, 56),   bcolor( 66,116, 48),   5,   0.3f);
        reg(SNOW,     "Snow",      true,  false, false, false, 0, bcolor(246,249,253),   bcolor(232,238,246),   bcolor(220,228,240),    1,   0.4f);
        reg(ICE,      "Ice",       true,  true,  false, false, 0, bcolor(180,222,242,200), bcolor(168,212,236,210), bcolor(160,204,230,210), 1, 0.8f);
        reg(LAVA,     "Lava",      false, true,  true,  true, 15, bcolor(236,118, 42),   bcolor(224, 88, 26),   bcolor(206, 70, 20),   3,  100.f);
        reg(IRON_ORE, "Iron Ore",  true,  false, false, false, 0, bcolor(152,148,150),   bcolor(142,138,141),   bcolor(132,128,131),   5,   2.5f);
        reg(GOLD_ORE, "Gold Ore",  true,  false, false, false, 0, bcolor(168,150, 96),   bcolor(156,139, 89),   bcolor(146,130, 83),   5,   3.0f);
        reg(BEDROCK,  "Bedrock",   true,  false, false, false, 0, bcolor( 64, 64, 72),   bcolor( 58, 58, 66),   bcolor( 52, 52, 59),   6,  -1.f);
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
