#!/usr/bin/env bash
# ============================================================
# Замеры горячих участков кадра на хосте, без устройства.
# Смотри tools/bench/bench.cpp — там же описано, как читать числа.
#
#   ./tools/bench/run.sh
#   ./tools/bench/run.sh меш        — только замеры с этой подстрокой
# ============================================================
set -eu

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$PROJ/build/bench"
TP="$PROJ/third_party"
SRC="$PROJ/app/src/main/cpp/src"
CXX="${CXX:-clang++}"

command -v "$CXX" >/dev/null 2>&1 || { echo "bench: не найден $CXX"; exit 1; }
[ -d "$TP/glm/glm" ] || {
    echo "bench: нет third_party/glm — запустите сначала ./tools/hostcheck/run.sh"
    exit 1; }

mkdir -p "$OUT"

# Заглушки Android/Vulkan: тот же файл, что у hostcheck.
"${CC:-clang}" -c -O2 -o "$OUT/stubs.o" "$PROJ/tools/hostcheck/stubs.c" \
    -I "$PROJ/tools/hostcheck/include" -isystem "$TP/Vulkan-Headers/include" \
    -DVK_USE_PLATFORM_ANDROID_KHR

# -O2: без оптимизации замеры меряют не код, а компилятор.
"$CXX" -std=c++20 -O2 -g0 \
    -D__ANDROID__ -DVK_USE_PLATFORM_ANDROID_KHR \
    -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DHOSTCHECK=1 \
    -I "$SRC" -I "$PROJ/tools/hostcheck/include" \
    -isystem "$TP/glm" -isystem "$TP/Vulkan-Headers/include" \
    -o "$OUT/bench" \
    "$PROJ/tools/bench/bench.cpp" \
    "$SRC"/world/{chunk,block,terrain,biome,noise,features,chunk_manager}.cpp \
    "$SRC"/render/mesh_builder.cpp \
    "$SRC"/world/ai/pathfinding.cpp \
    "$SRC"/audio/{audio_engine,sound_registry}.cpp \
    "$SRC"/core/{crashlog,job_system}.cpp \
    "$OUT/stubs.o" \
    -lpthread -ldl

"$OUT/bench" "$@"
