#!/usr/bin/env bash
# ============================================================
# Длинная игровая сессия на хосте: см. tools/soak/soak.cpp.
#
#   ./tools/soak/run.sh
#   ./tools/soak/run.sh --minutes 5 --profile sprint
# ============================================================
set -eu

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$PROJ/build/soak"
TP="$PROJ/third_party"
SRC="$PROJ/app/src/main/cpp/src"
CXX="${CXX:-clang++}"

command -v "$CXX" >/dev/null 2>&1 || { echo "soak: не найден $CXX"; exit 1; }
[ -d "$TP/glm/glm" ] || {
    echo "soak: нет third_party/glm — запустите сначала ./tools/hostcheck/run.sh"
    exit 1; }

mkdir -p "$OUT"

# Линкуемся с объектными файлами hostcheck, если они уже собраны:
# связи из сейвов тянутся через половину игры, и свой список
# исходников здесь пришлось бы дополнять каждый раз.
if [ ! -d "$PROJ/build/hostcheck/obj" ] || \
   [ -z "$(ls -A "$PROJ/build/hostcheck/obj" 2>/dev/null)" ]; then
    echo "==> Нет объектных файлов — запускаю hostcheck..."
    "$PROJ/tools/hostcheck/run.sh" --syntax >/dev/null 2>&1 || true
    if [ ! -d "$PROJ/build/hostcheck/obj" ]; then
        echo "soak: сначала выполните ./tools/hostcheck/run.sh"
        exit 1
    fi
fi

"$CXX" -std=c++20 -O2 -g0 \
    -D__ANDROID__ -DVK_USE_PLATFORM_ANDROID_KHR \
    -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DENTT_NO_ETO -DHOSTCHECK=1 \
    -I "$SRC" -I "$PROJ/tools/hostcheck/include" \
    -isystem "$TP/glm" -isystem "$TP/entt/include" -isystem "$TP/Vulkan-Headers/include" \
    -o "$OUT/soak" \
    "$PROJ/tools/soak/soak.cpp" "$PROJ/build/hostcheck"/obj/*.o \
    -lz -lpthread -ldl

"$OUT/soak" "$@"
