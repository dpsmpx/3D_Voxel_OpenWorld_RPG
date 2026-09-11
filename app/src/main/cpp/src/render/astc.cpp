/**
 * @file astc.cpp
 * @brief Рендер: чтение сжатых текстур ASTC.
 */
#include "astc.h"
#include "../core/log.h"
#include <cstring>

namespace render {

namespace {

/// Магическое число контейнера .astc, little-endian.
constexpr u32 ASTC_MAGIC = 0x5CA1AB13u;

#pragma pack(push, 1)
struct AstcHeader {
    u8 magic[4];
    u8 blockX, blockY, blockZ;
    u8 dimX[3];   ///< размеры хранятся тремя байтами, little-endian
    u8 dimY[3];
    u8 dimZ[3];
};
#pragma pack(pop)
static_assert(sizeof(AstcHeader) == 16, "Заголовок .astc — ровно 16 байт");

inline u32 read24(const u8 v[3]) {
    return (u32)v[0] | ((u32)v[1] << 8) | ((u32)v[2] << 16);
}

} // namespace

AstcImage parseAstc(const void* bytes, usize size) {
    AstcImage img;
    if (!bytes || size <= sizeof(AstcHeader)) {
        LOGE("ASTC: файл короче заголовка (%zu байт)", size);
        return img;
    }

    AstcHeader h{};
    std::memcpy(&h, bytes, sizeof(h));

    const u32 magic = (u32)h.magic[0] | ((u32)h.magic[1] << 8)
                    | ((u32)h.magic[2] << 16) | ((u32)h.magic[3] << 24);
    if (magic != ASTC_MAGIC) {
        LOGE("ASTC: неверная сигнатура 0x%08X", magic);
        return img;
    }

    const u32 w = read24(h.dimX);
    const u32 hh = read24(h.dimY);
    const u32 d = read24(h.dimZ);
    if (w == 0 || hh == 0 || d != 1) {
        LOGE("ASTC: неподдерживаемые размеры %ux%ux%u", w, hh, d);
        return img;
    }
    if (h.blockZ != 1) {
        LOGE("ASTC: 3D-блоки не поддерживаются");
        return img;
    }

    // Полное число блоков с округлением вверх — так же считает astcenc.
    const u32 blocksX = (w  + h.blockX - 1) / h.blockX;
    const u32 blocksY = (hh + h.blockY - 1) / h.blockY;
    const usize expected = (usize)blocksX * blocksY * 16;   // блок ASTC — 16 байт

    const usize payload = size - sizeof(AstcHeader);
    if (payload < expected) {
        LOGE("ASTC: данных %zu байт, ожидалось %zu", payload, expected);
        return img;
    }

    img.width  = w;
    img.height = hh;
    img.blockX = h.blockX;
    img.blockY = h.blockY;
    img.data.resize(expected);
    std::memcpy(img.data.data(), (const u8*)bytes + sizeof(AstcHeader), expected);
    return img;
}

AstcImage loadAstcAsset(AAssetManager* mgr, const char* path) {
    AstcImage img;
    if (!mgr) return img;

    AAsset* a = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
    if (!a) {
        // Не ошибка: сжатого атласа может не быть, тогда рендер
        // соберёт процедурный RGBA8.
        LOGI("ASTC: ассет %s отсутствует", path);
        return img;
    }

    const usize size = (usize)AAsset_getLength(a);
    const void* data = AAsset_getBuffer(a);
    img = parseAstc(data, size);
    AAsset_close(a);

    if (img.valid())
        LOGI("ASTC: %s загружен, %ux%u, блок %ux%u, %zu байт",
             path, img.width, img.height, img.blockX, img.blockY, img.data.size());
    return img;
}

} // namespace render
