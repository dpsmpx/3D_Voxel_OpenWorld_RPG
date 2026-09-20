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
            d.colorTop = top;
            d.colorSide = side;
            d.colorBottom = bottom;
            d.hardness = hardness;
            d.toolFlags = 0;
            inst.registerBlock(id, d);
        };

        // Один материал — один цвет. Верх, бок и низ различаются
        // только там, где это и правда разный материал: трава на
        // земле, спил на коре. Всё остальное различает освещённость
        // грани, и она задана в одном месте — FACE_LIGHT в
        // shaders/voxel.frag.
        //
        // Палитра подбиралась по трём правилам: соседние материалы
        // должны различаться по СВЕТЛОТЕ, а не только по тону (иначе
        // в тени они сливаются); ни один цвет не выкручен в чистый
        // спектр, чтобы освещение оставалось читаемым; руда обязана
        // быть видна на породе с десятка блоков — текстуры, по
        // которой её раньше узнавали бы, здесь нет, и весь блок
        // окрашен в цвет своего металла.
        //
        //  ID          имя        тв.  прз.  жид.  свеч. св.  верх                    бок                     низ                     прочн.
        reg(AIR,      "Air",       false, true,  false, false, 0, bcolor(  0,  0,  0,0), bcolor(  0,  0,  0,0), bcolor(  0,  0,  0,0),  0.0f);
        reg(STONE,    "Stone",     true,  false, false, false, 0, bcolor(142,144,152),   bcolor(142,144,152),   bcolor(142,144,152),   1.5f);
        reg(DIRT,     "Dirt",      true,  false, false, false, 0, bcolor(122, 89, 56),   bcolor(122, 89, 56),   bcolor(122, 89, 56),   0.6f);
        reg(GRASS,    "Grass",     true,  false, false, false, 0, bcolor(124,186, 84),   bcolor(122, 89, 56),   bcolor(122, 89, 56),   0.6f);
        reg(SAND,     "Sand",      true,  false, false, false, 0, bcolor(222,204,146),   bcolor(222,204,146),   bcolor(222,204,146),   0.5f);
        reg(WATER,    "Water",     false, true,  true,  false, 0, bcolor( 58,117,188,160), bcolor( 58,117,188,160), bcolor( 58,117,188,160), 100.f);
        reg(WOOD,     "Wood",      true,  false, false, false, 0, bcolor(158,122, 74),   bcolor(104, 74, 44),   bcolor(158,122, 74),   1.2f);
        reg(LEAVES,   "Leaves",    true,  true,  false, false, 0, bcolor( 84,143, 60),   bcolor( 84,143, 60),   bcolor( 84,143, 60),   0.3f);
        reg(SNOW,     "Snow",      true,  false, false, false, 0, bcolor(243,247,251),   bcolor(243,247,251),   bcolor(243,247,251),   0.4f);
        reg(ICE,      "Ice",       true,  true,  false, false, 0, bcolor(174,217,239,205), bcolor(174,217,239,205), bcolor(174,217,239,205), 0.8f);
        reg(LAVA,     "Lava",      false, true,  true,  true, 15, bcolor(230,103, 34),   bcolor(230,103, 34),   bcolor(230,103, 34),  100.f);
        // Руды: не «камень с крапинами», а свой цвет на весь блок.
        // При одном цвете на материал серая руда на серой породе
        // перестаёт быть видна вовсе — раньше её выдавала крапчатость.
        reg(IRON_ORE, "Iron Ore",  true,  false, false, false, 0, bcolor(176,152,134),   bcolor(176,152,134),   bcolor(176,152,134),   2.5f);
        reg(GOLD_ORE, "Gold Ore",  true,  false, false, false, 0, bcolor(204,174, 84),   bcolor(204,174, 84),   bcolor(204,174, 84),   3.0f);
        reg(BEDROCK,  "Bedrock",   true,  false, false, false, 0, bcolor( 60, 60, 68),   bcolor( 60, 60, 68),   bcolor( 60, 60, 68),  -1.f);

        // Строительные материалы. Подобраны по тому же правилу, что и
        // всё остальное: соседние по месту материалы обязаны
        // различаться СВЕТЛОТОЙ. Доска стоит рядом с соломой и с
        // камнем фундамента, солома — на фоне неба и листвы.
        reg(PLANK,    "Plank",     true,  false, false, false, 0, bcolor(198,164,116),   bcolor(198,164,116),   bcolor(198,164,116),   1.0f);
        reg(THATCH,   "Thatch",    true,  false, false, false, 0, bcolor(176,134, 52),   bcolor(162,122, 46),   bcolor(162,122, 46),   0.4f);
        // Стекло прозрачно, но твёрдо: сквозь него видно, а войти
        // нельзя. Мешер строит грань на границе с прозрачным, поэтому
        // окно в стене не дырявит дом.
        reg(GLASS,    "Glass",     true,  true,  false, false, 0, bcolor(190,218,232,110), bcolor(190,218,232,110), bcolor(190,218,232,110), 0.3f);
        // Фонарь светит. Раньше в середине деревянного дома лежала
        // лужа ЛАВЫ — она и была «факелом».
        reg(LANTERN,  "Lantern",   true,  false, false, true, 13, bcolor(255,198, 96),   bcolor(236,176, 78),   bcolor(236,176, 78),   0.5f);

        // Кактус. Зелень холоднее и темнее травы и листвы: он стоит на
        // песке, а песок — самый светлый материал в игре, и по тому же
        // правилу светлоты кактус обязан быть заметно темнее подложки.
        // Макушка чуть светлее бока — ровно как спил у дерева, и по
        // той же причине: это и правда другая грань материала.
        reg(CACTUS,   "Cactus",    true,  false, false, false, 0, bcolor( 96,150, 88),   bcolor( 68,118, 66),   bcolor( 68,118, 66),   0.4f);

        // Тёсаный камень. Светлее и теплее дикого: обтёсанный скол
        // ловит свет иначе, и ровно по этому его и отличают от скалы
        // — а отличать надо, иначе каменная деревня не читается.
        reg(BRICK,    "Brick",     true,  false, false, false, 0, bcolor(176,170,160),   bcolor(158,152,142),   bcolor(140,134,126),   1.5f);

        // Факел. Не твёрдый и без столкновения: сквозь него ходят, и
        // поставленный в узком ходе он этот ход не перекрывает.
        // Прозрачным помечен по той же причине, что листва и стекло —
        // чтобы мешер не склеивал его грани с соседями; альфа при
        // этом полная, и рисуется он непрозрачным.
        //
        // Верхняя грань — пламя, боковая — древко: свет берётся из
        // цвета ВЕРХНЕЙ грани, и «цветом огня» тут светит именно
        // огонь, а не палка.
        reg(TORCH,    "Torch",     false, true,  false, true, 14, bcolor(255,186, 84),   bcolor(126, 88, 52),   bcolor(126, 88, 52),   0.1f);
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
