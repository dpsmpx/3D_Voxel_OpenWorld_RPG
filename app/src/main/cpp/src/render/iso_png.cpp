/**
 * @file iso_png.cpp
 * @brief Запись PNG. Тонкая надстройка над уже линкуемым zlib.
 */
#include "iso_png.h"
#include "../save/zlib_util.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace render {

namespace {

void put32(std::vector<u8>& v, u32 x) {
    v.push_back((u8)(x >> 24));
    v.push_back((u8)(x >> 16));
    v.push_back((u8)(x >> 8));
    v.push_back((u8)x);
}

/// Блок PNG: длина, тип, данные, CRC по типу и данным.
void putChunk(std::vector<u8>& out, const char type[4],
              const u8* data, usize len)
{
    put32(out, (u32)len);
    const usize crcFrom = out.size();
    for (int i = 0; i < 4; ++i) out.push_back((u8)type[i]);
    out.insert(out.end(), data, data + len);
    const u32 crc = save::crc32_compute(out.data() + crcFrom, 4 + len);
    put32(out, crc);
}

/// Выбор фильтра строки.
///
/// PNG фильтрует каждую строку до сжатия, и выбор фильтра — это
/// единственное, что отделяет файл на десять мегабайт от файла на
/// два. Правило стандартное и дешёвое: берём тот фильтр, у которого
/// сумма модулей байтов наименьшая — у него меньше энтропия, и zlib
/// с ним справляется лучше.
///
/// Трёх фильтров хватает. Paeth и Average дороже, а на изометрии с
/// её крупными плоскими гранями выигрывают считанные проценты.
enum class Filter : u8 { None = 0, Sub = 1, Up = 2 };

u64 scoreRow(const std::vector<u8>& row) {
    u64 s = 0;
    // Знаковая трактовка: так считает сам стандарт.
    for (u8 b : row) s += (u64)(b < 128 ? b : 256 - b);
    return s;
}

} // namespace

std::vector<u8> encodePng(const u8* rgb, u32 w, u32 h) {
    if (!rgb || w == 0 || h == 0) return {};

    constexpr u32 CH = 3;                 // RGB, 8 бит на канал
    const usize stride = (usize)w * CH;

    // ---- фильтрация ----
    std::vector<u8> raw;
    raw.reserve((stride + 1) * h);

    std::vector<u8> fNone(stride), fSub(stride), fUp(stride);
    for (u32 y = 0; y < h; ++y) {
        const u8* cur  = rgb + (usize)y * stride;
        const u8* prev = y ? rgb + (usize)(y - 1) * stride : nullptr;

        for (usize i = 0; i < stride; ++i) {
            const u8 a = i >= CH ? cur[i - CH] : 0;      // левый пиксель
            const u8 b = prev ? prev[i] : 0;             // верхний
            fNone[i] = cur[i];
            fSub[i]  = (u8)(cur[i] - a);
            fUp[i]   = (u8)(cur[i] - b);
        }

        const u64 sNone = scoreRow(fNone);
        const u64 sSub  = scoreRow(fSub);
        const u64 sUp   = scoreRow(fUp);

        Filter best = Filter::None;
        const std::vector<u8>* bestRow = &fNone;
        u64 bestScore = sNone;
        if (sSub < bestScore) { best = Filter::Sub; bestRow = &fSub; bestScore = sSub; }
        if (sUp  < bestScore) { best = Filter::Up;  bestRow = &fUp;  bestScore = sUp;  }

        raw.push_back((u8)best);
        raw.insert(raw.end(), bestRow->begin(), bestRow->end());
    }

    const std::vector<u8> z = save::zcompress(raw, 6);
    if (z.empty()) {
        LOGE("PNG: zlib не сжал %u x %u", w, h);
        return {};
    }

    // ---- файл ----
    std::vector<u8> out;
    out.reserve(z.size() + 128);

    static const u8 SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    out.insert(out.end(), SIG, SIG + 8);

    u8 ihdr[13];
    ihdr[0] = (u8)(w >> 24); ihdr[1] = (u8)(w >> 16);
    ihdr[2] = (u8)(w >> 8);  ihdr[3] = (u8)w;
    ihdr[4] = (u8)(h >> 24); ihdr[5] = (u8)(h >> 16);
    ihdr[6] = (u8)(h >> 8);  ihdr[7] = (u8)h;
    ihdr[8]  = 8;   // бит на канал
    ihdr[9]  = 2;   // тип цвета: истинный цвет без альфы
    ihdr[10] = 0;   // сжатие: deflate, другого в PNG нет
    ihdr[11] = 0;   // фильтрация: базовый набор
    ihdr[12] = 0;   // без чересстрочности
    putChunk(out, "IHDR", ihdr, sizeof(ihdr));

    putChunk(out, "IDAT", z.data(), z.size());
    putChunk(out, "IEND", nullptr, 0);
    return out;
}

bool writePngFile(const char* path, const u8* rgb, u32 w, u32 h) {
    if (!path || !*path) return false;
    const std::vector<u8> png = encodePng(rgb, w, h);
    if (png.empty()) return false;

    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        LOGW("PNG: не открыть %s", path);
        return false;
    }
    const usize wrote = std::fwrite(png.data(), 1, png.size(), f);
    // Место могло кончиться на середине: оборванный файл не откроется
    // ни у кого, и узнать об этом лучше здесь, чем в галерее.
    const bool ok = (wrote == png.size());
    if (std::fclose(f) != 0 || !ok) {
        LOGE("PNG: записано %zu из %zu байт в %s", wrote, png.size(), path);
        std::remove(path);
        return false;
    }
    return true;
}

} // namespace render
