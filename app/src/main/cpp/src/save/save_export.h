/**
 * @file save_export.h
 * @brief Сохранения: выгрузка мира наружу и загрузка его обратно.
 */
#pragma once
#include "save_slot.h"
#include "../core/types.h"
#include <string>
#include <vector>

namespace save {

/// Чем кончилась выгрузка или загрузка.
enum class TransferStatus : u8 {
    Ok = 0,
    NoSource,     ///< нечего выгружать: слот пуст
    NoTarget,     ///< некуда: общий каталог недоступен
    ReadError,
    WriteError,
    BadFile,      ///< это не сейв: не тот магический номер или версия
};

const char* transferStatusString(TransferStatus s);

/// Расширение выгруженного мира.
///
/// Своё, а не `.vxs`: выгруженный файл — это сейв ВМЕСТЕ с
/// метаданными, одним куском. Два файла рядом игрок разнёс бы по
/// разным папкам при первой же пересылке, и мир приехал бы без имени.
constexpr const char* EXPORT_EXT = ".vxworld";

/// Куда складывать выгруженные миры внутри общего каталога.
constexpr const char* EXPORT_SUBDIR = "worlds";

/// Имя файла для мира: имя мира, приведённое к безопасному виду.
///
/// Кириллица, пробелы и двоеточия в именах файлов на чужих
/// файловых системах кончаются по-разному — от искажения до отказа.
/// Поэтому наружу уходит латиница, цифры, дефис и подчёркивание, а
/// настоящее имя мира едет ВНУТРИ файла.
std::string exportFileName(const SlotMeta& meta);

/// Общий каталог для выгруженных миров, уже созданный.
/// Пустая строка, если ни один не открылся.
std::string exportDir(const char* internalDataPath,
                      const char* externalDataPath);

/// Выгрузить слот одним файлом.
/// outPath — куда легло; заполняется и нужен, чтобы сказать игроку.
TransferStatus exportSlot(const SaveSlot& slot, const std::string& dir,
                          std::string& outPath);

/// Забрать выгруженный файл обратно в слот.
///
/// Слот перезаписывается целиком — и сейв, и метаданные. Спрашивать
/// подтверждение — дело вызывающего: здесь про то, что было в слоте,
/// знать уже поздно.
TransferStatus importSlot(const std::string& file, const SaveSlot& slot);

/// Список выгруженных миров в каталоге, отсортированный по имени.
std::vector<std::string> listExports(const std::string& dir);

} // namespace save
