/**
 * @file debug_scene.cpp
 * @brief Минимальная детерминированная сцена для сверки рендеров.
 */
#include "debug_scene.h"
#include "block.h"

namespace world {

void buildMinimalScene(Chunk& c) {
    // Чистый лист. Чанк и так обнулён при создании, но сцену могут
    // строить поверх уже сгенерированного чанка.
    for (i32 y = 0; y < CHUNK_SIZE_Y; ++y)
        for (i32 z = 0; z < CHUNK_SIZE; ++z)
            for (i32 x = 0; x < CHUNK_SIZE; ++x)
                c.setUnlocked(x, y, z, AIR);

    const i32 G = SCENE_GROUND_Y;

    // ---- ровная земля ----
    // Камень до G-4, земля до G-1, трава на G-1. Верх травы — ровно
    // на высоте G, то есть пол сцены.
    for (i32 z = 0; z < CHUNK_SIZE; ++z)
        for (i32 x = 0; x < CHUNK_SIZE; ++x) {
            for (i32 y = 0; y < G - 4; ++y) c.setUnlocked(x, y, z, STONE);
            for (i32 y = G - 4; y < G - 1; ++y) c.setUnlocked(x, y, z, DIRT);
            c.setUnlocked(x, G - 1, z, GRASS);
        }

    // Только в нулевом чанке — приметы. У соседей остаётся ровная
    // земля: она и служит фоном, и даёт центральному чанку соседей.
    if (c.coord.x != 0 || c.coord.z != 0) return;

    // ---- один поднятый блок ----
    c.setUnlocked(SCENE_BLOCK_X, G, SCENE_BLOCK_Z, STONE);

    // ---- одна лесенка ----
    // Ступенька на блок: по x от X0 до X1 высота растёт на единицу.
    // Верх каждой ступени — трава, бок — земля: так в кадре видно и
    // верхние грани, и боковые, и переход между ними.
    for (i32 x = SCENE_SLOPE_X0; x <= SCENE_SLOPE_X1; ++x) {
        const i32 top = G + (x - SCENE_SLOPE_X0);
        for (i32 z = 4; z <= 11; ++z) {
            for (i32 y = G; y < top; ++y) c.setUnlocked(x, y, z, DIRT);
            c.setUnlocked(x, top, z, GRASS);
        }
    }

    // ---- одна вода ----
    // Котлован на два блока вниз, вода до уровня земли: верхняя грань
    // воды оказывается вровень с травой, как в настоящем озере.
    for (i32 z = SCENE_POOL_Z0; z <= SCENE_POOL_Z1; ++z)
        for (i32 x = SCENE_POOL_X0; x <= SCENE_POOL_X1; ++x) {
            // Дно — камень: по нему сквозь воду видно, что
            // полупрозрачный проход читает глубину, а не затирает её.
            c.setUnlocked(x, G - 3, z, STONE);
            c.setUnlocked(x, G - 2, z, WATER);
            c.setUnlocked(x, G - 1, z, WATER);
        }

    // ---- одно дерево ----
    const i32 tx = SCENE_TREE_X, tz = SCENE_TREE_Z;
    for (i32 y = G; y < G + 4; ++y) c.setUnlocked(tx, y, tz, WOOD);
    for (i32 dy = 0; dy <= 2; ++dy)
        for (i32 dz = -2; dz <= 2; ++dz)
            for (i32 dx = -2; dx <= 2; ++dx) {
                // Углы верхнего и нижнего слоя срезаем — иначе крона
                // куб, и по ней не видно ни затенения углов, ни того,
                // как мешер режет ступеньки.
                const i32 r = dx * dx + dz * dz;
                if (dy != 1 && r > 4) continue;
                if (r > 8) continue;
                const i32 x = tx + dx, z = tz + dz, y = G + 3 + dy;
                if (c.at(x, y, z) != AIR) continue;
                c.setUnlocked(x, y, z, LEAVES);
            }
}

} // namespace world
