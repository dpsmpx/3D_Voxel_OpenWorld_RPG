# ============================================================
#  Отпечаток сборки — считается НА КАЖДУЮ сборку, не при настройке
# ============================================================
#
# Раньше `git rev-parse` стоял прямо в CMakeLists и выполнялся на этапе
# конфигурации. CMake настраивается один раз и переиспользуется, так что
# метка застревала: журнал с устройства показывал давнишний коммит, и по
# нему нельзя было понять, какая сборка на самом деле запущена. Ровно
# так и вышло — сборку со старым шейдером журнал подписал чужим SHA.
#
# Метка помечается «грязной», если рабочее дерево отличается от коммита:
# APK, собранный поверх незакоммиченных правок, — это не тот коммит, и
# подписывать его чистым SHA значит врать.
#
# Файл переписывается только при изменении: иначе каждая сборка
# пересобирала бы всё, что его включает.

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

if(NOT SHA)
    set(SHA "без git")
else()
    execute_process(
        COMMAND git status --porcelain
        WORKING_DIRECTORY ${SRC}
        OUTPUT_VARIABLE DIRT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(DIRT)
        set(SHA "${SHA}-грязный")
    endif()
endif()

set(TEXT "#pragma once\n#define VOXEL_BUILD_SHA \"${SHA}\"\n")

set(OLD "")
if(EXISTS ${OUT})
    file(READ ${OUT} OLD)
endif()
if(NOT OLD STREQUAL TEXT)
    file(WRITE ${OUT} "${TEXT}")
endif()
