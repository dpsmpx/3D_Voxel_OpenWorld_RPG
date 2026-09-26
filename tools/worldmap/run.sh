#!/usr/bin/env bash
# ============================================================
# Карта мира по слоям и цифры генератора. Смотри worldmap.cpp.
#
#   ./tools/worldmap/run.sh --seed 1 --layer all --out build/worldmap/s1
#   ./tools/worldmap/run.sh --seed 1 --layer none --stats
# ============================================================
set -eu

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$PROJ/build/worldmap"
TP="$PROJ/third_party"
SRC="$PROJ/app/src/main/cpp/src"
CXX="${CXX:-clang++}"

command -v "$CXX" >/dev/null 2>&1 || { echo "worldmap: не найден $CXX"; exit 1; }
[ -d "$TP/glm/glm" ] || {
    echo "worldmap: нет third_party/glm — запустите сначала ./tools/hostcheck/run.sh"
    exit 1; }
mkdir -p "$OUT"

"${CC:-clang}" -c -O2 -o "$OUT/stubs.o" "$PROJ/tools/hostcheck/stubs.c" \
    -I "$PROJ/tools/hostcheck/include" -isystem "$TP/Vulkan-Headers/include" \
    -DVK_USE_PLATFORM_ANDROID_KHR

# -O2: время генерации чанка, снятое с неоптимизированной сборки,
# меряет компилятор, а не генератор.
"$CXX" -std=c++20 -O2 -g0 \
    -D__ANDROID__ -DVK_USE_PLATFORM_ANDROID_KHR \
    -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DHOSTCHECK=1 \
    -I "$SRC" -I "$PROJ/tools/hostcheck/include" \
    -isystem "$TP/glm" -isystem "$TP/Vulkan-Headers/include" \
    -o "$OUT/worldmap" \
    "$PROJ/tools/worldmap/worldmap.cpp" \
    "$SRC"/world/{chunk,block,terrain,biome,noise,features,hydrology,landform,flora,debug_scene}.cpp \
    "$SRC"/render/iso_png.cpp \
    "$SRC"/save/zlib_util.cpp \
    "$SRC"/config/settings.cpp \
    "$SRC"/core/{crashlog,job_system,shared_dir}.cpp \
    "$OUT/stubs.o" \
    -lz -lpthread -ldl

cd "$PROJ"
"$OUT/worldmap" "$@"
