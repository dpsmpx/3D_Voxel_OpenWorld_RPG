/**
 * @file astc.h
 * @brief Рендер: чтение сжатых текстур ASTC.
 */
#pragma once
#include "../core/types.h"
#include <android/asset_manager.h>
#include <vector>

namespace render {

// ============================================================
// Контейнер .astc — формат, который пишет astcenc.
//
// 16 байт заголовка, дальше — блоки подряд, без выравнивания и
// мип-уровней. Для атласа блоков этого достаточно: мип-цепочку
// для сжатой текстуры всё равно нельзя построить на устройстве
// (vkCmdBlitImage не работает со сжатыми форматами), поэтому
// атлас грузится одним уровнем.
// ============================================================
struct AstcImage {
    u32 width = 0;
    u32 height = 0;
    u32 blockX = 0;   ///< размер блока по X, для ASTC 4x4 — 4
    u32 blockY = 0;
    std::vector<u8> data;   ///< сжатые блоки

    bool valid() const { return width > 0 && height > 0 && !data.empty(); }
};

/// Читает .astc из буфера в памяти.
/// @return изображение с valid() == false, если заголовок не тот.
AstcImage parseAstc(const void* bytes, usize size);

/// Читает .astc из ассета APK.
/// @param mgr  менеджер ассетов из ANativeActivity
/// @param path путь вида "textures/blocks.astc"
/// @return изображение с valid() == false, если ассета нет
AstcImage loadAstcAsset(AAssetManager* mgr, const char* path);

} // namespace render
