/**
 * @file iso_png.h
 * @brief Запись PNG. Тонкая надстройка над уже линкуемым zlib.
 *
 * Своя запись, а не библиотека, по двум причинам. Первая: всё, что
 * нужно PNG, в проекте уже есть — zlib для IDAT и его же crc32 для
 * контрольных сумм каждого блока (`save::zcompress`,
 * `save::crc32_compute`). Вторая: единственный формат, который нам
 * нужен, — восемь бит на канал, три канала, без чересстрочности; это
 * сотня строк, а не зависимость.
 *
 * Формат: signature, IHDR, IDAT, IEND. Ровно по спецификации, потому
 * что открывать файл будет не игра, а галерея телефона и любой
 * просмотрщик на другом конце.
 */
#pragma once
#include "../core/types.h"
#include <vector>

namespace render {

/// Кодирует RGB8 в PNG.
///
/// @param rgb     w*h*3 байта, построчно сверху вниз.
/// @return        готовый файл; пустой вектор — ошибка.
std::vector<u8> encodePng(const u8* rgb, u32 w, u32 h);

/// Пишет PNG на диск. Возвращает false, если не удалось.
bool writePngFile(const char* path, const u8* rgb, u32 w, u32 h);

} // namespace render
