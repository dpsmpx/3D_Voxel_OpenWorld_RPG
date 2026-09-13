/**
 * @file diag_build_check.cpp
 * @brief Сверка двух конфигураций сборки: обычной и диагностической.
 *
 * Собирается ДВАЖДЫ — с -DVOXEL_DEBUG_SCENE=1 и без него, — потому что
 * решение принимается на этапе компиляции и внутри одного бинарника
 * проверить обе стороны нельзя.
 *
 * Смысл: диагностический APK обязан запускаться сценой сам, без
 * settings.cfg, а уже лежащий на устройстве файл с debug_scene = false
 * не должен уметь выключить его обратно.
 */
#include "config/settings.h"
#include <cstdio>
#include <cstdlib>

static int failed = 0;

static void check(bool ok, const char* what) {
    std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failed;
}

int main() {
#ifdef VOXEL_DEBUG_SCENE
    std::printf("  сборка: диагностическая (-DVOXEL_DEBUG_SCENE=1)\n");
    check(config::DIAGNOSTIC_BUILD, "признак диагностической сборки поднят");

    config::Settings s;
    check(s.debugScene, "сцена включена по умолчанию, без settings.cfg");

    // Файл с выключенной сценой не должен пересилить флаг сборки.
    s.debugScene = false;
    s.clamp();
    check(s.debugScene, "настройкой диагностическую сборку не выключить");
#else
    std::printf("  сборка: обычная\n");
    check(!config::DIAGNOSTIC_BUILD, "признак диагностической сборки опущен");

    config::Settings s;
    check(!s.debugScene, "в обычной сборке сцена выключена по умолчанию");

    // А вот включить её настройкой обычная сборка обязана позволять.
    s.debugScene = true;
    s.clamp();
    check(s.debugScene, "в обычной сборке сцену можно включить настройкой");
#endif
    return failed ? 1 : 0;
}
