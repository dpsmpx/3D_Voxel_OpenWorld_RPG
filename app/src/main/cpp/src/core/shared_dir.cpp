/**
 * @file shared_dir.cpp
 * @brief Каталоги, из которых файл видят ДРУГИЕ программы.
 */
#include "shared_dir.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sys {

void packageFromPath(const char* path, char* out, usize outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!path) return;
    static const char* markers[] = {"/Android/data/", "/data/data/", "/data/user/"};
    for (const char* m : markers) {
        const char* found = std::strstr(path, m);
        if (!found) continue;
        const char* rest = found + std::strlen(m);
        // У /data/user/ сначала идёт номер пользователя.
        if (std::strcmp(m, "/data/user/") == 0) {
            while (*rest && *rest != '/') ++rest;
            if (*rest == '/') ++rest;
        }
        usize n = 0;
        while (rest[n] && rest[n] != '/' && n + 1 < outSize) { out[n] = rest[n]; ++n; }
        out[n] = '\0';
        if (out[0]) return;
    }
}

u32 sharedDirCandidates(const char* internalDataPath,
                        const char* externalDataPath,
                        char out[][SHARED_PATH_CAP], u32 cap)
{
    if (!out || cap == 0) return 0;
    u32 n = 0;
    auto push = [&](const char* fmt, const char* a, const char* b) {
        if (n >= cap) return;
        char buf[SHARED_PATH_CAP];
        std::snprintf(buf, sizeof(buf), fmt, a, b);
        // Повторы ни к чему: корни на прошивках совпадают чаще, чем
        // различаются, и один и тот же каталог пробовался бы трижды.
        for (u32 i = 0; i < n; ++i)
            if (std::strcmp(out[i], buf) == 0) return;
        std::snprintf(out[n], SHARED_PATH_CAP, "%s", buf);
        ++n;
    };

    char pkg[256];
    packageFromPath(externalDataPath, pkg, sizeof(pkg));
    if (!pkg[0]) packageFromPath(internalDataPath, pkg, sizeof(pkg));

    if (pkg[0]) {
        // Если externalDataPath всё же есть, его префикс — самый
        // верный корень: система назвала его сама.
        if (externalDataPath && *externalDataPath) {
            const char* found = std::strstr(externalDataPath, "/Android/data/");
            if (found) {
                char root[SHARED_PATH_CAP];
                std::snprintf(root, sizeof(root), "%.*s",
                              (int)(found - externalDataPath), externalDataPath);
                push("%s/Android/media/%s", root, pkg);
            }
        }
        const char* env = std::getenv("EXTERNAL_STORAGE");
        const char* roots[] = { env, "/storage/emulated/0", "/sdcard" };
        for (const char* root : roots) {
            if (!root || !*root) continue;
            push("%s/Android/media/%s", root, pkg);
        }
    }

    // Запасной вариант: собственный внешний каталог. Termux туда не
    // дотягивается, но файловые менеджеры через системный выбор
    // папки — да.
    if (externalDataPath && *externalDataPath) push("%s%s", externalDataPath, "");
    return n;
}

} // namespace sys
