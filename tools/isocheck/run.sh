#!/usr/bin/env bash
# ============================================================
# Изометрический снимок мира на хосте: настоящий Vulkan (lavapipe),
# настоящий мир, настоящий PNG. Тот же класс render::IsoSnapshot, что
# и в игре, те же шейдеры и тот же проход рендера.
#
# Нужен затем, что числами изометрию проверяет tools/hostcheck, а
# посмотреть на неё глазами можно только по картинке — и устройства
# для этого иметь не обязательно.
#
#   ./tools/isocheck/run.sh
#   ./tools/isocheck/run.sh --all-views --size 100 --px 12
# ============================================================
set -eu

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$PROJ/build/isocheck"
TP="$PROJ/third_party"
SRC="$PROJ/app/src/main/cpp/src"
CXX="${CXX:-clang++}"

command -v "$CXX" >/dev/null 2>&1 || { echo "isocheck: не найден $CXX"; exit 1; }
[ -d "$TP/glm/glm" ] || { echo "isocheck: нет third_party/glm — сначала ./tools/hostcheck/run.sh"; exit 1; }

for icd in /usr/share/vulkan/icd.d/lvp_icd.json \
           /usr/share/vulkan/icd.d/lvp_icd.x86_64.json; do
    [ -f "$icd" ] && export VK_ICD_FILENAMES="$icd" && break
done
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    echo "isocheck: не найден программный драйвер Vulkan (mesa-vulkan-drivers, lavapipe)"
    exit 1
fi

mkdir -p "$OUT/assets/shaders"

. "$PROJ/tools/glsl-cc.sh"
glsl_find || { echo "isocheck: $(glsl_hint)"; exit 1; }

echo "==> Шейдеры ($GLSL_CC)..."
# В каталог инструмента, не в поставку: см. тот же довод в vkcheck.
for f in "$PROJ"/app/src/main/cpp/shaders/*.vert "$PROJ"/app/src/main/cpp/shaders/*.frag; do
    [ -e "$f" ] || continue
    glsl_compile "$f" "$OUT/assets/shaders/$(basename "$f").spv" >/dev/null
done

echo "==> Сборка..."
"$CXX" -std=c++20 -O1 -g \
    -D__ANDROID__ -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DHOSTCHECK=1 \
    -DVK_USE_PLATFORM_ANDROID_KHR \
    -I "$SRC" -I "$PROJ/tools/hostcheck/include" -isystem "$TP/glm" \
    -isystem "$TP/entt/include" -isystem "$TP/Vulkan-Headers/include" \
    -o "$OUT/isocheck" \
    "$PROJ/tools/isocheck/isocheck.cpp" \
    "$SRC"/vk/{vk_buffer,vk_pipeline,vk_shader,vk_descriptors,vk_renderpass}.cpp \
    "$SRC"/render/{mesh_builder,voxel_pipeline,iso_projection,iso_png,iso_snapshot}.cpp \
    "$SRC"/world/{chunk,chunk_manager,block,terrain,biome,noise,features,hydrology,debug_scene}.cpp \
    "$SRC"/core/{crashlog,shared_dir,job_system}.cpp \
    "$SRC"/save/zlib_util.cpp \
    "$SRC"/config/settings.cpp \
    -lvulkan -lz -ldl -lpthread

echo "==> Снимок..."
cd "$PROJ"
"$OUT/isocheck" --out "$OUT/iso.png" "$@"
