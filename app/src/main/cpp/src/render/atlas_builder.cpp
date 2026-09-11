#include "atlas_builder.h"
#include "../world/block.h"
#include <random>
#include <cstring>

namespace render {

namespace {

// Простой hash от координат для детерминированного шума
inline u32 hash2(u32 x, u32 y) {
    u32 h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

struct TileSpec {
    u8 r, g, b, a;
    u8 noise;      // амплитуда шума 0..64
    bool topGreen = false; // для травы: сверху зелёная полоса
};

TileSpec tileForBlock(world::BlockId id) {
    using namespace world;
    switch (id) {
        case STONE:    return { 105, 105, 110, 255, 12 };
        case DIRT:     return { 110,  80,  50, 255, 16 };
        case GRASS:    return { 110,  80,  50, 255, 16, true };
        case SAND:     return { 215, 200, 130, 255, 12 };
        case WATER:    return {  50, 100, 180, 170,  8 };
        case WOOD:     return { 120,  80,  40, 255, 18 };
        case LEAVES:   return {  60, 120,  50, 255, 24 };
        case SNOW:     return { 240, 245, 250, 255,  6 };
        case ICE:      return { 180, 220, 240, 200,  8 };
        case LAVA:     return { 230,  80,  20, 255, 30 };
        case IRON_ORE: return { 130, 130, 130, 255, 14 };
        case GOLD_ORE: return { 140, 130, 100, 255, 14 };
        case BEDROCK:  return {  50,  50,  55, 255, 20 };
        default:       return { 200, 200, 200, 255, 20 };
    }
}

// Для руд — добавить цветные вкрапления в тайл
bool isOreWithFlecks(world::BlockId id) {
    return id == world::IRON_ORE || id == world::GOLD_ORE;
}

u32 fleckColorFor(world::BlockId id) {
    if (id == world::IRON_ORE) return 0xFFA0A0B0; // светло-серый
    if (id == world::GOLD_ORE) return 0xFFFFD060; // золотой
    return 0xFFFFFFFF;
}

} // namespace

AtlasData buildProceduralAtlas() {
    AtlasData a;
    a.pixels.resize(a.width * a.height * 4, 0);

    for (u32 blockId = 0; blockId < world::BLOCK_COUNT; ++blockId) {
        // В атласе тайл id размещается по (id % 16, id / 16).
        // Мы не используем AIR=0 — оставим его чёрным.
        if (blockId == world::AIR) continue;

        u32 col = blockId % a.columns;
        u32 row = blockId / a.columns;
        u32 baseX = col * a.tileSize;
        u32 baseY = row * a.tileSize;

        TileSpec spec = tileForBlock((world::BlockId)blockId);
        bool ore = isOreWithFlecks((world::BlockId)blockId);
        u32 fleck = fleckColorFor((world::BlockId)blockId);

        for (u32 y = 0; y < a.tileSize; ++y) {
            for (u32 x = 0; x < a.tileSize; ++x) {
                u8 r = spec.r, g = spec.g, b = spec.b;

                // Шум
                if (spec.noise) {
                    i32 n = (i32)(hash2(col * 1000 + x, row * 1000 + y) % (spec.noise * 2 + 1))
                          - (i32)spec.noise;
                    r = (u8)std::max(0, std::min(255, (i32)r + n));
                    g = (u8)std::max(0, std::min(255, (i32)g + n));
                    b = (u8)std::max(0, std::min(255, (i32)b + n));
                }

                // Трава: сверху 4 пикселя — зелёная полоса
                if (spec.topGreen && y < 4) {
                    r = 80; g = 140; b = 60;
                }

                // Руды: вкрапления
                if (ore && (hash2(col * 7 + x, row * 13 + y) % 40) == 0) {
                    r = (fleck >> 16) & 0xFF;
                    g = (fleck >>  8) & 0xFF;
                    b = (fleck >>  0) & 0xFF;
                }

                u32 idx = ((baseY + y) * a.width + (baseX + x)) * 4;
                a.pixels[idx + 0] = r;
                a.pixels[idx + 1] = g;
                a.pixels[idx + 2] = b;
                a.pixels[idx + 3] = spec.a;
            }
        }
    }
    return a;
}

} // namespace render
