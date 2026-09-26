#!/usr/bin/env bash
# ============================================================
# Стоимость полноэкранного прохода настоящими шейдерами игры.
# Смотри tools/gpubench/gpubench.cpp — там же, как читать числа.
#
#   ./tools/gpubench/run.sh
#   ./tools/gpubench/run.sh --size 2306 1080 --iters 8
# ============================================================
set -eu

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$PROJ/build/gpubench"
TP="$PROJ/third_party"
CXX="${CXX:-clang++}"

for icd in /usr/share/vulkan/icd.d/lvp_icd.json \
           /usr/share/vulkan/icd.d/lvp_icd.x86_64.json; do
    [ -f "$icd" ] && export VK_ICD_FILENAMES="$icd" && break
done
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    echo "gpubench: нет программного драйвера Vulkan"
    echo "          apt-get install -y mesa-vulkan-drivers libvulkan-dev"
    exit 1
fi

mkdir -p "$OUT/assets/shaders"

# Шейдеры игры собираются в каталог ИНСТРУМЕНТА, а не в поставку.
#
# Раньше они клались прямо в app/src/main/assets/shaders — туда же,
# откуда gradle пакует APK. Инструмент тем самым переписывал содержимое
# будущей поставки, и однажды APK уехал на устройство со старым
# voxel.frag: сборка была новая, шейдер в ней — прошлый, а замер на
# устройстве молча повторил прежние числа.
# Компилятор — через общий помощник, как у vkcheck и hostcheck: на
# машине бывает то glslc, то glslangValidator.
. "$PROJ/tools/glsl-cc.sh"
glsl_find || { echo "gpubench: $(glsl_hint)"; exit 1; }
OPT=""; [ "$GLSL_CC_KIND" = glslc ] && OPT="-O"
for f in "$PROJ"/app/src/main/cpp/shaders/sky.vert "$PROJ"/app/src/main/cpp/shaders/sky.frag \
         "$PROJ"/app/src/main/cpp/shaders/voxel.frag; do
    glsl_compile "$f" "$OUT/assets/shaders/$(basename "$f").spv" $OPT >/dev/null
done

# Опорный шейдер кладётся в каталог сборки, а НЕ в ассеты игры.
#
# Сначала он писался рядом с шейдерами игры — и уехал в APK: gradle
# пакует весь assets/shaders целиком. Инструмент не имеет права
# добавлять файлы в поставку.
glsl_compile "$PROJ/tools/gpubench/flat.frag" "$OUT/gpubench_flat.frag.spv" $OPT >/dev/null
glsl_compile "$PROJ/tools/gpubench/voxel_probe.vert" "$OUT/gpubench_voxel_probe.vert.spv" $OPT >/dev/null
glsl_compile "$PROJ/tools/gpubench/voxel_probe.vert" "$OUT/gpubench_water_probe.vert.spv" $OPT -DWATER >/dev/null

"$CXX" -std=c++20 -O2 -g0 \
    -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL \
    -I "$PROJ/app/src/main/cpp/src" \
    -isystem "$TP/glm" \
    -o "$OUT/gpubench" "$PROJ/tools/gpubench/gpubench.cpp" \
    -lvulkan

cd "$PROJ"
"$OUT/gpubench" --assets "$OUT/assets" --bench-assets "$OUT" "$@"
