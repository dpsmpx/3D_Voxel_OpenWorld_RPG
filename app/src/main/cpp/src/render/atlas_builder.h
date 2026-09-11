/**
 * @file atlas_builder.h
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include <vector>

namespace render {

/// Процедурный атлас 16×16 тайлов по 32×32 пикселя = 512×512 RGBA8.
/// Генерирует текстуру для каждого ID блока с собственным цветом
/// и небольшим шумом для "шершавости". Заменяется на реальный
/// ассет в Phase 3 — интерфейс остаётся.
struct AtlasData {
    std::vector<u8> pixels;   // RGBA8, 512*512*4
    u32 width = 512;
    u32 height = 512;
    u32 tileSize = 32;
    u32 columns = 16;
};

/// Собирает атлас текстур блоков 16x16 тайлов процедурно,
/// без внешних файлов. Вызывается один раз при старте рендера.
AtlasData buildProceduralAtlas();

} // namespace render
