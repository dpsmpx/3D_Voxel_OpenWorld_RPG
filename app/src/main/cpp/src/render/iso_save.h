/**
 * @file iso_save.h
 * @brief Куда и под каким именем ложится изометрический снимок.
 *
 * Отдельно от самого рендера, потому что это про файловую систему
 * Android, а не про графику, — и потому что имя файла надо проверять
 * на хосте, без Vulkan и без телефона.
 */
#pragma once
#include "../core/types.h"
#include "iso_projection.h"
#include <string>
#include <vector>

namespace render {

/// Имя файла снимка.
///
/// В имени обязаны читаться все условия съёмки: из папки на сотню
/// карт иначе не выбрать нужную, а два снимка одного места с разных
/// сторон различаются только этим.
///
///   voxelrpg_map_x-42_z128_100b_north_20260920-143017.png
///            |     |      |     |     |
///            |     |      |     |     время съёмки
///            |     |      |     сторона обзора
///            |     |      размер области в БЛОКАХ
///            |     координаты центра — блок, в котором стоял игрок
///            что это вообще такое
///
/// Координаты идут со знаком и без пробелов: имя должно пережить
/// перенос на любой компьютер и любую файловую систему.
std::string isoFileName(i32 centerX, i32 centerZ, i32 sizeBlocks,
                        iso::View view, i64 unixTime);

/// Название стороны обзора для имени файла. Латиницей: имя файла
/// уезжает на другие машины, и кириллица там превращается в кашу.
const char* isoViewSlug(iso::View v);

/// Каталог для снимков — тот, который видно снаружи приложения.
///
/// Пробуются те же кандидаты, что и для журнала (`sys::sharedDirCandidates`):
/// первым идёт `Android/media/<пакет>`, потому что с Android 11
/// `Android/data` закрыт для других приложений, а `Android/media` —
/// нет. Ни одного дополнительного разрешения это не требует.
///
/// Возвращает пустую строку, если ни один каталог не удалось создать.
std::string isoSaveDir(const char* internalDataPath,
                       const char* externalDataPath);

/// Полный путь записанного файла или пустая строка при неудаче.
std::string saveIsoSnapshot(const char* internalDataPath,
                            const char* externalDataPath,
                            i32 centerX, i32 centerZ, i32 sizeBlocks,
                            iso::View view, i64 unixTime,
                            const u8* rgb, u32 w, u32 h);

} // namespace render
