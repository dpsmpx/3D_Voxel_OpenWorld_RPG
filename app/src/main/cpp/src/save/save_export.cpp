/**
 * @file save_export.cpp
 * @brief Сохранения: выгрузка мира наружу и загрузка его обратно.
 */
#include "save_export.h"
#include "save_format.h"
#include "../core/shared_dir.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>

namespace save {

namespace {

/// Заголовок выгруженного файла.
///
/// Свой, отдельный от заголовка сейва: в одном файле едут ДВА куска —
/// метаданные и сам сейв, — и без длин их не разделить. Проверять
/// версию сейва здесь нечем и не нужно: это делает загрузчик, и
/// делает лучше.
constexpr u32 EXPORT_MAGIC   = 0x584F5657u;   // "VWOX"
constexpr u32 EXPORT_VERSION = 1u;

bool readWhole(const std::string& path, std::vector<u8>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return false; }
    out.resize((usize)sz);
    const usize got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

bool writeWhole(const std::string& path, const u8* data, usize size) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const usize put = size ? std::fwrite(data, 1, size, f) : 0;
    std::fclose(f);
    if (put != size) { std::remove(path.c_str()); return false; }
    return true;
}

bool makeDir(const std::string& path) {
    if (::mkdir(path.c_str(), 0775) == 0) return true;
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void putU32(std::vector<u8>& v, u32 x) {
    v.push_back((u8)(x & 0xFF));
    v.push_back((u8)((x >> 8) & 0xFF));
    v.push_back((u8)((x >> 16) & 0xFF));
    v.push_back((u8)((x >> 24) & 0xFF));
}

u32 getU32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

} // namespace

const char* transferStatusString(TransferStatus s) {
    switch (s) {
        case TransferStatus::Ok:         return "ok";
        case TransferStatus::NoSource:   return "нечего выгружать";
        case TransferStatus::NoTarget:   return "некуда выгружать";
        case TransferStatus::ReadError:  return "файл не прочитан";
        case TransferStatus::WriteError: return "файл не записан";
        case TransferStatus::BadFile:    return "это не мир";
        default:                         return "?";
    }
}

std::string exportFileName(const SlotMeta& meta) {
    std::string safe;
    // Только то, что переживёт любую файловую систему и любую
    // пересылку. Настоящее имя мира едет внутри файла.
    for (usize i = 0; i < sizeof(meta.worldName) && meta.worldName[i]; ++i) {
        const char c = meta.worldName[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) safe.push_back(c);
        else if (c == ' ' && !safe.empty() && safe.back() != '_') safe.push_back('_');
    }
    while (!safe.empty() && safe.back() == '_') safe.pop_back();

    char buf[128];
    // Зерно в имени — не украшение: два мира с одним названием
    // затирали бы друг друга, а зерно у них разное всегда.
    std::snprintf(buf, sizeof(buf), "%s%s%08llX%s",
                  safe.c_str(), safe.empty() ? "world_" : "_",
                  (unsigned long long)(meta.seed & 0xFFFFFFFFull),
                  EXPORT_EXT);
    return std::string(buf);
}

std::string exportDir(const char* internalDataPath,
                      const char* externalDataPath)
{
    char dirs[8][sys::SHARED_PATH_CAP];
    const u32 n = sys::sharedDirCandidates(internalDataPath, externalDataPath,
                                           dirs, 8);
    for (u32 i = 0; i < n; ++i) {
        if (!makeDir(dirs[i])) continue;
        std::string sub = std::string(dirs[i]) + "/" + EXPORT_SUBDIR;
        if (makeDir(sub)) return sub;
    }
    // Ни один общий каталог не открылся — пусть уж ляжет внутрь:
    // оттуда его достанет хотя бы резервное копирование системы.
    if (internalDataPath && *internalDataPath) {
        std::string sub = std::string(internalDataPath) + "/" + EXPORT_SUBDIR;
        if (makeDir(sub)) return sub;
    }
    return std::string();
}

TransferStatus exportSlot(const SaveSlot& slot, const std::string& dir,
                          std::string& outPath)
{
    outPath.clear();
    if (dir.empty()) return TransferStatus::NoTarget;
    if (!slot.dataExists()) return TransferStatus::NoSource;

    SlotMeta meta{};
    if (!slot.readMeta(meta)) return TransferStatus::NoSource;

    std::vector<u8> body;
    if (!readWhole(slot.dataPath(), body)) return TransferStatus::ReadError;

    std::vector<u8> metaBytes;
    if (!readWhole(slot.metaPath(), metaBytes)) return TransferStatus::ReadError;

    std::vector<u8> file;
    file.reserve(body.size() + metaBytes.size() + 16);
    putU32(file, EXPORT_MAGIC);
    putU32(file, EXPORT_VERSION);
    putU32(file, (u32)metaBytes.size());
    putU32(file, (u32)body.size());
    file.insert(file.end(), metaBytes.begin(), metaBytes.end());
    file.insert(file.end(), body.begin(), body.end());

    outPath = dir + "/" + exportFileName(meta);
    if (!writeWhole(outPath, file.data(), file.size())) {
        outPath.clear();
        return TransferStatus::WriteError;
    }
    LOGI("мир выгружен: %s (%zu байт)", outPath.c_str(), file.size());
    return TransferStatus::Ok;
}

TransferStatus importSlot(const std::string& file, const SaveSlot& slot) {
    std::vector<u8> raw;
    if (!readWhole(file, raw)) return TransferStatus::ReadError;
    if (raw.size() < 16) return TransferStatus::BadFile;
    if (getU32(&raw[0]) != EXPORT_MAGIC) return TransferStatus::BadFile;
    if (getU32(&raw[4]) != EXPORT_VERSION) return TransferStatus::BadFile;

    const usize metaSize = (usize)getU32(&raw[8]);
    const usize bodySize = (usize)getU32(&raw[12]);
    if (16 + metaSize + bodySize != raw.size()) return TransferStatus::BadFile;

    // Сейв пишется ПЕРВЫМ, метаданные вторыми. Слот без метаданных
    // виден в списке как пустой, а метаданные без сейва — как мир,
    // который не открывается. Первое честнее.
    if (!writeWhole(slot.dataPath(), raw.data() + 16 + metaSize, bodySize))
        return TransferStatus::WriteError;
    if (!writeWhole(slot.metaPath(), raw.data() + 16, metaSize)) {
        std::remove(slot.dataPath().c_str());
        return TransferStatus::WriteError;
    }
    LOGI("мир загружен: %s -> %s", file.c_str(), slot.dataPath().c_str());
    return TransferStatus::Ok;
}

std::vector<std::string> listExports(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    const usize extLen = std::strlen(EXPORT_EXT);
    while (struct dirent* e = ::readdir(d)) {
        const usize n = std::strlen(e->d_name);
        if (n <= extLen) continue;
        if (std::strcmp(e->d_name + (n - extLen), EXPORT_EXT) != 0) continue;
        out.push_back(dir + "/" + e->d_name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace save
