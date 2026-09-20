/**
 * @file iso_save.cpp
 * @brief Куда и под каким именем ложится изометрический снимок.
 */
#include "iso_save.h"
#include "iso_png.h"
#include "../core/shared_dir.h"
#include "../core/log.h"
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace render {

namespace {

/// mkdir -p без вызова оболочки. Такой же, как в журнале аварий:
/// каталога снимков на свежем телефоне ещё нет.
bool makeDirs(const char* path) {
    char tmp[sys::SHARED_PATH_CAP];
    std::snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        ::mkdir(tmp, 0775);
        *p = '/';
    }
    return ::mkdir(tmp, 0775) == 0 || errno == EEXIST;
}

} // namespace

const char* isoViewSlug(iso::View v) {
    switch (v) {
        case iso::View::North: return "north";
        case iso::View::East:  return "east";
        case iso::View::South: return "south";
        case iso::View::West:  return "west";
        default:               return "north";
    }
}

std::string isoFileName(i32 centerX, i32 centerZ, i32 sizeBlocks,
                        iso::View view, i64 unixTime)
{
    // Время — местное: имя читает человек, а не машина.
    std::tm tmv{};
    const std::time_t t = (std::time_t)unixTime;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    char name[256];
    std::snprintf(name, sizeof(name),
                  "voxelrpg_map_x%d_z%d_%db_%s_%s.png",
                  centerX, centerZ, sizeBlocks, isoViewSlug(view), stamp);
    return std::string(name);
}

std::string isoSaveDir(const char* internalDataPath,
                       const char* externalDataPath)
{
    char dirs[8][sys::SHARED_PATH_CAP];
    const u32 n = sys::sharedDirCandidates(internalDataPath, externalDataPath,
                                           dirs, 8);
    for (u32 i = 0; i < n; ++i) {
        char full[sys::SHARED_PATH_CAP];
        std::snprintf(full, sizeof(full), "%s/maps", dirs[i]);
        if (!makeDirs(full)) continue;
        // Каталог создан — но записать в него мы можем и не мочь.
        // Проверяем делом, а не правами: на Android права каталога
        // рассказывают не всю правду.
        char probe[sys::SHARED_PATH_CAP + 16];
        std::snprintf(probe, sizeof(probe), "%s/.probe", full);
        if (std::FILE* f = std::fopen(probe, "wb")) {
            std::fclose(f);
            std::remove(probe);
            return std::string(full);
        }
    }
    LOGW("снимок: не нашлось каталога, доступного снаружи");
    return {};
}

std::string saveIsoSnapshot(const char* internalDataPath,
                            const char* externalDataPath,
                            i32 centerX, i32 centerZ, i32 sizeBlocks,
                            iso::View view, i64 unixTime,
                            const u8* rgb, u32 w, u32 h)
{
    const std::string dir = isoSaveDir(internalDataPath, externalDataPath);
    if (dir.empty()) return {};

    const std::string name = isoFileName(centerX, centerZ, sizeBlocks,
                                         view, unixTime);
    const std::string path = dir + "/" + name;
    if (!writePngFile(path.c_str(), rgb, w, h)) return {};
    LOGI("снимок сохранён: %s (%ux%u)", path.c_str(), w, h);
    return path;
}

} // namespace render
