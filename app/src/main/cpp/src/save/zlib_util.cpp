/**
 * @file zlib_util.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "zlib_util.h"
#include "../core/log.h"
#include <zlib.h>
#include <cstring>
#include <vector>

namespace save {

std::vector<u8> zcompress(const std::vector<u8>& in, int level) {
    if (in.empty()) return {};

    uLongf bound = compressBound((uLong)in.size());
    std::vector<u8> out(bound);

    int r = compress2(out.data(), &bound,
                      in.data(), (uLong)in.size(), level);
    if (r != Z_OK) {
        LOGE("zcompress: ошибка %d", r);
        return {};
    }
    out.resize(bound);
    return out;
}

std::vector<u8> zdecompress(const std::vector<u8>& in, u64 maxSize) {
    if (in.empty()) return {};

    u64 cap = (u64)in.size() * 4ull;
    if (cap < 4096) cap = 4096;
    if (cap > maxSize) cap = maxSize;

    for (int attempt = 0; attempt < 20; ++attempt) {
        std::vector<u8> out(cap);
        uLongf outLen = (uLongf)cap;

        int r = uncompress(out.data(), &outLen,
                           in.data(), (uLong)in.size());

        if (r == Z_OK) {
            out.resize(outLen);
            return out;
        }
        if (r == Z_BUF_ERROR) {
            cap *= 2;
            if (cap > maxSize) {
                cap = maxSize;
            }
            continue;
        }
        LOGE("zdecompress: ошибка %d", r);
        return {};
    }
    LOGE("zdecompress: превышен maxSize");
    return {};
}

u32 crc32_compute(const void* data, usize size) {
    return (u32)::crc32(0L, (const Bytef*)data, (uInt)size);
}

} // namespace save
