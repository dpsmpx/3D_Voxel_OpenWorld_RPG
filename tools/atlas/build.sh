#!/usr/bin/env bash
# ============================================================
# Сборка и запуск утилиты атласа на хосте.
#
#   ./tools/atlas/build.sh <каталог ассетов>
#
# Кладёт textures/blocks.astc в указанный каталог, если в системе
# есть astcenc. Иначе печатает предупреждение и выходит с кодом 0:
# ASTC — необязательная оптимизация, игра работает и без него.
# ============================================================
set -u

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
ASSETS="${1:-$PROJ/app/src/main/assets}"
SRC_DIR="$PROJ/app/src/main/cpp/src"
OUT="$PROJ/build/atlas"

. "$PROJ/tools/ndk-common.sh"

# Утилита атласа выполняется ЗДЕСЬ ЖЕ, сразу после сборки, поэтому
# нужен компилятор под текущую машину. Кросс-компилятор из NDK не
# подходит: он соберёт исполняемый файл под другую архитектуру,
# который тут же откажется запускаться.
if ! CXX="$(host_cxx)"; then
    echo "atlas: не найден рабочий C++ компилятор для этой машины."
    echo "       Termux: pkg install clang"
    exit 0
fi

mkdir -p "$OUT" "$ASSETS/textures"

echo "atlas: компилятор $CXX"
if ! "$CXX" -std=c++20 -O2 -I "$SRC_DIR" \
        -o "$OUT/atlas_tool" \
        "$PROJ/tools/atlas/main.cpp" "$SRC_DIR/render/atlas_builder.cpp" \
        2> "$OUT/build.err"; then
    echo "atlas: утилита не собралась:"
    head -20 "$OUT/build.err" | sed 's/^/    /'
    exit 1
fi

if ASTCENC_BIN="$(find_astcenc)"; then
    echo "atlas: кодировщик ASTC $ASTCENC_BIN"
    "$OUT/atlas_tool" "$OUT/blocks.tga" "$ASSETS/textures/blocks.astc" "$ASTCENC_BIN"
else
    # Это не ошибка сборки: атлас в ASTC — оптимизация памяти (ТЗ 3.3).
    # Без него игра собирает атлас процедурно при старте и работает.
    echo "atlas: кодировщик ASTC не найден — атлас останется RGBA8."
    echo "       Это не ошибка: игра соберёт его при старте, просто"
    echo "       текстуры займут вчетверо больше видеопамяти."
    echo "       В репозиториях Termux кодировщика нет ни под одним"
    echo "       именем. Собрать его из исходников ARM:"
    echo "           ./tools/build-astcenc.sh"
    echo "       (или указать готовый: ASTCENC=/путь/к/astcenc ./build.sh)"
    "$OUT/atlas_tool" "$OUT/blocks.tga"
fi
