#pragma once
#include "../core/types.h"
#include <vector>

namespace save {

// ============================================================
// Обёртки над системным libz (линкуется с -lz).
// ============================================================

// Сжать буфер. level 1..9 (по умолчанию 6 — компромисс).
// Возвращает пустой вектор при ошибке или пустом входе.
std::vector<u8> zcompress(const std::vector<u8>& in, int level = 6);

// Разжать. maxSize — верхняя граница результата (защита от бомб).
// Возвращает пустой вектор при ошибке.
std::vector<u8> zdecompress(const std::vector<u8>& in, u64 maxSize);

// CRC32 — быстрый, для проверки целостности тела.
u32 crc32_compute(const void* data, usize size);

} // namespace save
