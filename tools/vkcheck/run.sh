#!/usr/bin/env bash
# ============================================================
# Настоящий Vulkan на хосте: те же .spv, тот же конвейер и тот же
# проход рендера, что в APK. Кадр рисуется программной реализацией
# Vulkan (lavapipe) в память, слой проверки включён.
#
#   ./tools/vkcheck/run.sh
#   ./tools/vkcheck/run.sh --lod 2 --pos 52.8 1.1 --out /tmp/a.ppm
# ============================================================
set -eu

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$PROJ/build/vkcheck"
TP="$PROJ/third_party"
SRC="$PROJ/app/src/main/cpp/src"
CXX="${CXX:-clang++}"

command -v "$CXX" >/dev/null 2>&1 || { echo "vkcheck: не найден $CXX"; exit 1; }
[ -d "$TP/glm/glm" ] || { echo "vkcheck: нет third_party/glm — сначала ./tools/hostcheck/run.sh"; exit 1; }

# Программная реализация Vulkan. Без неё на машине сборки нет ни
# одного устройства, и проверять нечем.
for icd in /usr/share/vulkan/icd.d/lvp_icd.json \
           /usr/share/vulkan/icd.d/lvp_icd.x86_64.json; do
    [ -f "$icd" ] && export VK_ICD_FILENAMES="$icd" && break
done
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    echo "vkcheck: не найден программный драйвер Vulkan (mesa-vulkan-drivers, lavapipe)"
    echo "         установите: apt-get install -y mesa-vulkan-drivers vulkan-validationlayers"
    exit 1
fi

mkdir -p "$OUT"

echo "==> Шейдеры..."
mkdir -p "$PROJ/app/src/main/assets/shaders"
for f in "$PROJ"/app/src/main/cpp/shaders/*.vert "$PROJ"/app/src/main/cpp/shaders/*.frag; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"
    glslc "$f" -o "$PROJ/app/src/main/assets/shaders/$base.spv"
done

# Отладочные фрагментные шейдеры инструмента: вершинный при этом
# остаётся настоящим, игровым.
mkdir -p "$OUT/assets/shaders"
for m in 0 1 2 3 4 5; do
    glslc -DMODE=$m "$PROJ/tools/vkcheck/debug.frag" \
          -o "$OUT/assets/shaders/debug$m.frag.spv"
done

echo "==> Сборка..."
"$CXX" -std=c++20 -O1 -g \
    -D__ANDROID__ -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DHOSTCHECK=1 \
    -DVK_USE_PLATFORM_ANDROID_KHR \
    -I "$SRC" -I "$PROJ/tools/hostcheck/include" -isystem "$TP/glm" \
    -isystem "$TP/Vulkan-Headers/include" \
    -o "$OUT/vkcheck" \
    "$PROJ/tools/vkcheck/vkcheck.cpp" \
    "$SRC"/vk/{vk_buffer,vk_pipeline,vk_shader,vk_descriptors,vk_renderpass}.cpp \
    "$SRC"/render/{mesh_builder,voxel_pipeline}.cpp \
    "$SRC"/world/{chunk,block,terrain,biome,noise,features}.cpp \
    "$SRC"/core/crashlog.cpp \
    -lvulkan -ldl -lpthread

echo "==> Кадр..."
cd "$PROJ"
"$OUT/vkcheck" --out "$OUT/frame.ppm" --debug-assets "$OUT/assets" "$@" || rc=$?
rc=${rc:-0}

if command -v python3 >/dev/null 2>&1 && python3 -c "import PIL" 2>/dev/null; then
    python3 - "$OUT" <<'PY'
import sys
from PIL import Image
out = sys.argv[1]
try:
    Image.open(f"{out}/frame.ppm").save(f"{out}/frame.png")
    print(f"vkcheck: {out}/frame.png")
except Exception as e:
    print("vkcheck: png не сделан:", e)
PY
fi
exit $rc
