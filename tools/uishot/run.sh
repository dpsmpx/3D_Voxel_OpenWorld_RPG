#!/usr/bin/env bash
# Снимок интерфейса в PNG. Vulkan не нужен: кадр строится на процессоре.
set -u
PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$PROJ/build/uishot"
CPP="$PROJ/app/src/main/cpp"
TP="$PROJ/third_party"
mkdir -p "$OUT"

SRCS=(
  "$PROJ/tools/uishot/uishot.cpp"
  "$PROJ/tools/hostcheck/stubs.c"
)
# Остальное берём уже собранным hostcheck'ом: те же объектники, что
# проверяет CI, — снимок не имеет права собираться из другого кода.
OBJ="$PROJ/build/hostcheck/obj"
if [ ! -d "$OBJ" ]; then
  echo "uishot: сначала нужен прогон tools/hostcheck/run.sh --syntax"
  exit 1
fi

CXX="${CXX:-clang++}"
"$CXX" -std=c++20 -O1 -g -D__ANDROID__ \
  -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DHOSTCHECK=1 \
  -DVK_USE_PLATFORM_ANDROID_KHR \
  -I "$CPP/src" -I "$PROJ/tools/hostcheck/include" \
  -isystem "$TP/glm" -isystem "$TP/entt/include" \
  -isystem "$TP/Vulkan-Headers/include" \
  -o "$OUT/uishot" "$PROJ/tools/uishot/uishot.cpp" "$OBJ"/*.o \
  -lz -ldl -lpthread 2> "$OUT/build.err" || {
    echo "uishot: не собралось"; head -30 "$OUT/build.err" | sed 's/^/    /'; exit 1; }

"$OUT/uishot" "$@"
