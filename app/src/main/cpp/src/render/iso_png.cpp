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
#include <string>
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

    const std::string tmp = std::string(path) + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        LOGW("PNG: не открыть %s", tmp.c_str());
        return false;
    }
    const usize wrote = std::fwrite(png.data(), 1, png.size(), f);
    // Место могло кончиться на середине: оборванный файл не откроется
    // ни у кого, и узнать об этом лучше здесь, чем в галерее.
    const bool ok = (wrote == png.size());
    if (std::fclose(f) != 0 || !ok) {
        LOGE("PNG: записано %zu из %zu байт в %s", wrote, png.size(), path);
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path) != 0) {
        LOGE("PNG: не переименовать %s", tmp.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

namespace {

u32 get32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

u8 paeth(u8 a, u8 b, u8 c) {
    const int p = (int)a + (int)b - (int)c;
    const int pa = std::abs(p - (int)a);
    const int pb = std::abs(p - (int)b);
    const int pc = std::abs(p - (int)c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

} // namespace

bool decodePng(const std::vector<u8>& png, std::vector<u8>& rgb,
               u32& w, u32& h)
{
    static const u8 SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (png.size() < 8 + 25 || std::memcmp(png.data(), SIG, 8) != 0) return false;

    // Предел по стороне — тот же, что у снимка: больше игра не пишет,
    // а испорченный заголовок иначе заказал бы гигабайты.
    constexpr u32 MAX_SIDE = 8192;

    u32 ch = 0;
    w = h = 0;
    std::vector<u8> z;
    bool haveHeader = false, ended = false;
    usize pos = 8;
    while (pos + 12 <= png.size() && !ended) {
        const u32 len = get32(&png[pos]);
        if ((u64)pos + 12 + len > png.size()) return false;
        const u8* type = &png[pos + 4];
        const u8* data = &png[pos + 8];
        const u32 crc = get32(data + len);
        if (save::crc32_compute(type, 4 + (usize)len) != crc) return false;

        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) return false;
            w = get32(data);
            h = get32(data + 4);
            const u8 depth = data[8], color = data[9];
            const u8 comp = data[10], filt = data[11], lace = data[12];
            if (depth != 8 || comp != 0 || filt != 0 || lace != 0) return false;
            if (color == 2) ch = 3;
            else if (color == 6) ch = 4;
            else return false;
            if (w == 0 || h == 0 || w > MAX_SIDE || h > MAX_SIDE) return false;
            haveHeader = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            if (!haveHeader) return false;
            z.insert(z.end(), data, data + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            ended = true;
        }
        pos += 12 + (usize)len;
    }
    if (!haveHeader || !ended || z.empty()) return false;

    const usize stride = (usize)w * ch;
    const u64 rawSize = (u64)(stride + 1) * h;
    const std::vector<u8> raw = save::zdecompress(z, rawSize);
    if (raw.size() != rawSize) return false;

    // ---- снятие фильтров ----
    std::vector<u8> img((usize)stride * h);
    for (u32 y = 0; y < h; ++y) {
        const u8 f = raw[(usize)y * (stride + 1)];
        const u8* src = &raw[(usize)y * (stride + 1) + 1];
        u8* cur = &img[(usize)y * stride];
        const u8* prev = y ? &img[(usize)(y - 1) * stride] : nullptr;
        for (usize i = 0; i < stride; ++i) {
            const u8 a = i >= ch ? cur[i - ch] : 0;
            const u8 b = prev ? prev[i] : 0;
            const u8 c = (prev && i >= ch) ? prev[i - ch] : 0;
            u8 v = src[i];
            switch (f) {
                case 0: break;
                case 1: v = (u8)(v + a); break;
                case 2: v = (u8)(v + b); break;
                case 3: v = (u8)(v + (u8)(((u32)a + (u32)b) / 2)); break;
                case 4: v = (u8)(v + paeth(a, b, c)); break;
                default: return false;
            }
            cur[i] = v;
        }
    }

    if (ch == 3) { rgb.swap(img); return true; }
    rgb.resize((usize)w * h * 3);
    for (usize i = 0, n = (usize)w * h; i < n; ++i) {
        rgb[i * 3 + 0] = img[i * 4 + 0];
        rgb[i * 3 + 1] = img[i * 4 + 1];
        rgb[i * 3 + 2] = img[i * 4 + 2];
    }
    return true;
}

bool readPngFile(const char* path, std::vector<u8>& rgb, u32& w, u32& h) {
    if (!path || !*path) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    // Превью — сотни килобайт; сорок мегабайт — заведомо не наш файл.
    if (sz <= 0 || sz > (40L << 20)) { std::fclose(f); return false; }
    std::vector<u8> buf((usize)sz);
    const usize got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return false;
    return decodePng(buf, rgb, w, h);
}

} // namespace render
