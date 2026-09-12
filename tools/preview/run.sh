#!/usr/bin/env bash
# ============================================================
# Рендер мира в картинку на хосте, без устройства и без Vulkan.
# Все ключи уходят в саму программу, см. tools/preview/preview.cpp.
#
#   ./tools/preview/run.sh
#   ./tools/preview/run.sh --lod 2
#   ./tools/preview/run.sh --debug ao --out ao.ppm
# ============================================================
set -eu

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$PROJ/build/preview"
TP="$PROJ/third_party"
CXX="${CXX:-clang++}"

command -v "$CXX" >/dev/null 2>&1 || { echo "preview: не найден $CXX"; exit 1; }
[ -d "$TP/glm/glm" ] || {
    echo "preview: нет third_party/glm — запустите сначала ./tools/hostcheck/run.sh"
    exit 1; }

mkdir -p "$OUT"
SRC="$PROJ/app/src/main/cpp/src"

# Заглушки Android/Vulkan: тот же файл, что у hostcheck.
"${CC:-clang}" -c -O0 -o "$OUT/stubs.o" "$PROJ/tools/hostcheck/stubs.c" \
    -I "$PROJ/tools/hostcheck/include" -isystem "$TP/Vulkan-Headers/include" \
    -DVK_USE_PLATFORM_ANDROID_KHR

"$CXX" -std=c++20 -O2 \
    -D__ANDROID__ -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DHOSTCHECK=1 \
    -I "$SRC" -I "$PROJ/tools/hostcheck/include" -isystem "$TP/glm" \
    -o "$OUT/preview" \
    "$PROJ/tools/preview/preview.cpp" \
    "$SRC"/world/{chunk,block,terrain,biome,noise,features}.cpp \
    "$SRC"/render/mesh_builder.cpp \
    "$SRC"/core/crashlog.cpp \
    "$OUT/stubs.o" \
    -ldl -lpthread

cd "$OUT"
./preview --out "$OUT/preview.ppm" "$@"

# PPM открывается не везде; если есть чем — переложим в PNG.
if command -v python3 >/dev/null 2>&1 &&
   python3 -c "import PIL" 2>/dev/null; then
    python3 - "$OUT" <<'PY'
import sys
from PIL import Image
out = sys.argv[1]
Image.open(f"{out}/preview.ppm").save(f"{out}/preview.png")
print(f"preview: {out}/preview.png")
PY
fi
