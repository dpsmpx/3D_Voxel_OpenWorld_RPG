#!/usr/bin/env bash
# ============================================================
# Настоящий Vulkan на хосте: те же .spv, тот же конвейер и тот же
# проход рендера, что в APK. Кадр рисуется программной реализацией
# Vulkan (lavapipe) в память, слой проверки включён.
#
#   ./tools/vkcheck/run.sh
#   ./tools/vkcheck/run.sh --pos 52.8 1.1 --out /tmp/a.ppm
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
# Собираются в каталог ИНСТРУМЕНТА, а не в app/src/main/assets.
#
# Раньше — прямо в поставку, откуда gradle пакует APK. Этого хватило,
# чтобы отправить на устройство APK со старым voxel.frag: прогон A/B
# оставил там шейдеры из git stash, gradle их и упаковал, а замер на
# устройстве молча повторил прежние числа. Инструмент не имеет права
# писать в поставку — ни файлов, ни своих.
mkdir -p "$OUT/assets/shaders"
for f in "$PROJ"/app/src/main/cpp/shaders/*.vert "$PROJ"/app/src/main/cpp/shaders/*.frag; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"
    glslc "$f" -o "$OUT/assets/shaders/$base.spv"
done

# Отладочные фрагментные шейдеры инструмента: вершинный при этом
# остаётся настоящим, игровым.
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
    "$SRC"/world/{chunk,block,terrain,biome,noise,features,debug_scene}.cpp \
    "$SRC"/core/crashlog.cpp \
    "$SRC"/core/shared_dir.cpp \
    -lvulkan -ldl -lpthread

echo "==> Кадр..."
cd "$PROJ"
"$OUT/vkcheck" --out "$OUT/frame.ppm" --assets "$OUT/assets" \
              --debug-assets "$OUT/assets" "$@" || rc=$?
rc=${rc:-0}

# PPM в PNG — своими руками, без Pillow.
#
# Раньше здесь звался Pillow под защитой "import PIL". Защита
# оказалась не той: сам пакет PIL импортируется, а нужный ему
# PIL.Image тянет бинарный модуль, и вот его может не быть в системе
# (или он собран под другую версию python — ровно так и вышло).
# Питон падал, а при set -e вместе с ним падала ВСЯ проверка графики,
# хотя кадр к тому времени был отрисован и проверен, и слой проверки
# не сказал ни слова.
#
# Картинка нужна человеку, а не проверке, поэтому её отсутствие не
# может ронять проверку. Кодировщик PNG без внешних пакетов влезает в
# два десятка строк стандартной библиотеки — зависимость того не стоила.
if command -v python3 >/dev/null 2>&1; then
    python3 "$PROJ/tools/vkcheck/ppm2png.py" "$OUT/frame.ppm" "$OUT/frame.png" \
        || echo "vkcheck: png не сделан (кадр в .ppm на месте)"
fi
exit $rc
