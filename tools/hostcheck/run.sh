#!/usr/bin/env bash
# ============================================================
# hostcheck — компиляция и линковка всего нативного кода на
# обычном Linux-хосте, без Android NDK.
#
# Назначение: поймать ошибки компиляции и неразрешённые символы
# до того, как код попадёт на устройство. NDK нужен только для
# настоящей сборки APK (build.sh); здесь используются
# заголовки-заглушки Android/AAudio из tools/hostcheck/include
# и заглушки символов Vulkan/Android из stubs.c.
#
#   ./tools/hostcheck/run.sh            — компиляция + линковка
#   ./tools/hostcheck/run.sh --syntax   — только компиляция
#
# Возвращает 0, только если всё собралось и слинковалось.
# ============================================================
set -u

PROJ="$(cd "$(dirname "$0")/../.." && pwd)"
CPP_DIR="$PROJ/app/src/main/cpp"
SRC_DIR="$CPP_DIR/src"
TP="$PROJ/third_party"
OUT="$PROJ/build/hostcheck"

SYNTAX_ONLY=0
[ "${1:-}" = "--syntax" ] && SYNTAX_ONLY=1

CXX="${CXX:-clang++}"
CC="${CC:-clang}"

command -v "$CXX" >/dev/null 2>&1 || { echo "hostcheck: не найден $CXX"; exit 1; }

# ---- зависимости ----
mkdir -p "$TP"
if [ ! -d "$TP/glm/glm" ]; then
    echo "==> Клонирую glm..."
    git clone --depth=1 -q https://github.com/g-truc/glm.git "$TP/glm" || {
        echo "hostcheck: не удалось получить glm"; exit 1; }
fi
if [ ! -f "$TP/entt/include/entt/entt.hpp" ]; then
    echo "==> Клонирую EnTT..."
    rm -rf "$TP/entt-src"
    git clone --depth=1 -q --branch v3.13.2 \
        https://github.com/skypjack/entt.git "$TP/entt-src" || {
        echo "hostcheck: не удалось получить EnTT"; exit 1; }
    mkdir -p "$TP/entt/include/entt"
    cp "$TP/entt-src/single_include/entt/entt.hpp" "$TP/entt/include/entt/"
    rm -rf "$TP/entt-src"
fi
if [ ! -d "$TP/Vulkan-Headers/include" ]; then
    echo "==> Клонирую Vulkan-Headers..."
    git clone --depth=1 -q --branch v1.3.280 \
        https://github.com/KhronosGroup/Vulkan-Headers.git "$TP/Vulkan-Headers" || {
        echo "hostcheck: не удалось получить Vulkan-Headers"; exit 1; }
fi

# ---- список исходников: диск против CMakeLists ----
mapfile -t DISK < <(cd "$CPP_DIR" && find src -name '*.cpp' | sort)
mapfile -t CMAKE_LIST < <(sed -n '/add_library(native-lib SHARED/,/^)/p' \
    "$CPP_DIR/CMakeLists.txt" | grep -oE 'src/[A-Za-z0-9_/.-]+\.cpp' | sort)

MISSING=$(comm -13 <(printf '%s\n' "${DISK[@]}") <(printf '%s\n' "${CMAKE_LIST[@]}"))
ORPHAN=$(comm -23 <(printf '%s\n' "${DISK[@]}") <(printf '%s\n' "${CMAKE_LIST[@]}"))
FAIL=0
if [ -n "$MISSING" ]; then
    echo "✗ В CMakeLists.txt перечислены отсутствующие файлы:"; echo "$MISSING" | sed 's/^/    /'
    FAIL=1
fi
if [ -n "$ORPHAN" ]; then
    echo "✗ Файлы на диске не перечислены в CMakeLists.txt:"; echo "$ORPHAN" | sed 's/^/    /'
    FAIL=1
fi
[ "$FAIL" -eq 1 ] && exit 1

# ---- компиляция ----
rm -rf "$OUT"; mkdir -p "$OUT/obj"
FLAGS=(
    -std=c++20 -fsyntax-only
    -D__ANDROID__ -DVK_USE_PLATFORM_ANDROID_KHR -DGLM_FORCE_DEPTH_ZERO_TO_ONE
    -DGLM_ENABLE_EXPERIMENTAL -DENTT_NO_ETO -DHOSTCHECK=1
    -I "$SRC_DIR" -I "$PROJ/tools/hostcheck/include"
    -isystem "$TP/glm" -isystem "$TP/entt/include" -isystem "$TP/Vulkan-Headers/include"
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    # Как в настоящей сборке: без исключений и RTTI. Иначе хост
    # принимает try/throw, dynamic_cast и typeid, а устройство — нет.
    -fno-exceptions -fno-rtti
)
COMPILE_FLAGS=("${FLAGS[@]}")
if [ "$SYNTAX_ONLY" -eq 0 ]; then
    COMPILE_FLAGS=("${FLAGS[@]/-fsyntax-only/-c}")
    COMPILE_FLAGS+=(-fPIC -O1 -g0)
fi

NPROC="$(nproc 2>/dev/null || echo 4)"
LOG="$OUT/compile.log"; : > "$LOG"
echo "==> Компиляция ${#DISK[@]} файлов (-j$NPROC)..."

compile_one() {
    local f="$1"
    local obj="$OUT/obj/$(echo "${f%.cpp}" | tr '/' '_').o"
    local args=("${COMPILE_FLAGS[@]}")
    [ "$SYNTAX_ONLY" -eq 0 ] && args+=(-o "$obj")
    if ! "$CXX" "${args[@]}" "$CPP_DIR/$f" 2> "$OUT/$(echo "$f" | tr '/' '_').err"; then
        { echo "########## $f"; cat "$OUT/$(echo "$f" | tr '/' '_').err"; } >> "$LOG"
        return 1
    fi
    # предупреждения тоже собираем
    if [ -s "$OUT/$(echo "$f" | tr '/' '_').err" ]; then
        { echo "########## $f"; cat "$OUT/$(echo "$f" | tr '/' '_').err"; } >> "$LOG"
    fi
    return 0
}
export -f compile_one
export CXX OUT CPP_DIR SYNTAX_ONLY LOG
export COMPILE_FLAGS_STR="${COMPILE_FLAGS[*]}"

BAD=0
pids=(); running=0
for f in "${DISK[@]}"; do
    (
        obj="$OUT/obj/$(echo "${f%.cpp}" | tr '/' '_').o"
        err="$OUT/$(echo "$f" | tr '/' '_').err"
        args=(${COMPILE_FLAGS_STR})
        [ "$SYNTAX_ONLY" -eq 0 ] && args+=(-o "$obj")
        "$CXX" "${args[@]}" "$CPP_DIR/$f" 2> "$err" || echo "$f" >> "$OUT/failed.txt"
    ) &
    pids+=($!); running=$((running+1))
    if [ "$running" -ge "$NPROC" ]; then wait -n 2>/dev/null || wait; running=$((running-1)); fi
done
wait

for f in "${DISK[@]}"; do
    err="$OUT/$(echo "$f" | tr '/' '_').err"
    [ -s "$err" ] && { echo "########## $f"; cat "$err"; } >> "$LOG"
done

ERRORS=$(grep -c "error:" "$LOG" 2>/dev/null || echo 0)
WARNS=$(grep -c "warning:" "$LOG" 2>/dev/null || echo 0)
if [ -s "$OUT/failed.txt" ]; then
    BADN=$(sort -u "$OUT/failed.txt" | wc -l)
    echo "✗ Не скомпилировалось файлов: $BADN из ${#DISK[@]}, ошибок: $ERRORS"
    grep "error:" "$LOG" | head -40
    echo "    полный лог: $LOG"
    exit 1
fi
echo "✓ Скомпилировано ${#DISK[@]}/${#DISK[@]}, ошибок: 0, предупреждений: $WARNS"
# Держим ноль намеренно: каждое предупреждение здесь оказалось либо
# мёртвым кодом, либо забытой строкой, и разбирать их поштучно проще,
# пока их ноль, чем когда накопилось два десятка. Отключается на
# случай другой версии компилятора: HOSTCHECK_WARN_FATAL=0.
if [ "$WARNS" -gt 0 ]; then
    echo "    предупреждения: $LOG"
    grep -E "warning:" "$LOG" | sed 's|^|    |' | head -20
    if [ "${HOSTCHECK_WARN_FATAL:-1}" -eq 1 ]; then
        echo "✗ Предупреждения считаются ошибкой (HOSTCHECK_WARN_FATAL=0 чтобы снять)"
        exit 1
    fi
fi

[ "$SYNTAX_ONLY" -eq 1 ] && exit 0

# ---- линковка ----
echo "==> Линковка..."
"$CC" -c -fPIC -O0 -o "$OUT/obj/_stubs.o" "$PROJ/tools/hostcheck/stubs.c" \
     -I "$PROJ/tools/hostcheck/include" -isystem "$TP/Vulkan-Headers/include" \
     -DVK_USE_PLATFORM_ANDROID_KHR 2> "$OUT/stubs.err" || {
        echo "✗ Не скомпилировались заглушки"; cat "$OUT/stubs.err"; exit 1; }

if ! "$CXX" -shared -o "$OUT/libnative-lib.so" "$OUT"/obj/*.o -lz -lpthread -ldl \
        -Wl,--no-undefined 2> "$OUT/link.err"; then
    echo "✗ Линковка не прошла:"
    grep -E "undefined|error" "$OUT/link.err" | sed 's/^/    /' | head -40
    echo "    полный лог: $OUT/link.err"
    exit 1
fi

SIZE=$(du -h "$OUT/libnative-lib.so" | cut -f1)
echo "✓ Слинковано: $OUT/libnative-lib.so ($SIZE), неразрешённых символов нет"

# ---- шейдеры ----
# Ошибка в GLSL не видна ни компилятору C++, ни тестам логики:
# она всплыла бы только на устройстве чёрным экраном.
if command -v glslc >/dev/null 2>&1; then
    echo "==> Компиляция шейдеров..."
    mkdir -p "$OUT/shaders"
    SH_FAIL=0
    for f in "$CPP_DIR"/shaders/*.vert "$CPP_DIR"/shaders/*.frag; do
        [ -f "$f" ] || continue
        if ! glslc -O "$f" -o "$OUT/shaders/$(basename "$f").spv" \
                2>> "$OUT/shaders.err"; then
            SH_FAIL=1
        fi
    done
    if [ "$SH_FAIL" -ne 0 ]; then
        echo "✗ Шейдеры не скомпилировались:"
        sed 's/^/    /' "$OUT/shaders.err" | head -30
        exit 1
    fi
    # Предупреждения тоже считаем дефектом: раньше они молча падали
    # в лог, и warning про precision нашёлся только на устройстве.
    if [ -s "$OUT/shaders.err" ]; then
        echo "✗ Шейдеры собрались с предупреждениями:"
        sed 's/^/    /' "$OUT/shaders.err" | head -30
        exit 1
    fi
    echo "✓ Шейдеров скомпилировано: $(ls "$OUT/shaders" | wc -l), предупреждений нет"
else
    echo "! glslc не найден — шейдеры не проверены (pkg install shaderc)"
fi

# ---- сверка вершинных форматов ----
# Проверяет, что таблицы vk::VertexAttr в C++ совпадают с входами
# шейдеров. Расхождение здесь компилятор не поймает.
echo "==> Вершинные форматы..."
if ! python3 "$PROJ/tools/hostcheck/check_shaders.py"; then
    echo "✗ Вершинные форматы расходятся с шейдерами"
    exit 1
fi

# ---- включай то, что используешь ----
# Транзитивные включения стандартной библиотеки различаются между её
# версиями: std::clamp без <algorithm> собирался здесь и валил CI.
echo "==> Недостающие #include..."
if ! python3 "$PROJ/tools/hostcheck/check_includes.py"; then
    exit 1
fi

# ---- уровни Android API ----
# Хост компилирует с заглушками, атрибутов доступности из настоящих
# заголовков NDK здесь нет. Без этой проверки «появился в API 26» и
# «убран из NDK r27» выясняются только на устройстве.
echo "==> Уровни Android API..."
if ! python3 "$PROJ/tools/hostcheck/check_android_api.py"; then
    exit 1
fi

# ---- тесты логики ----
echo "==> Тесты..."
TEST_SRCS=(
    "$PROJ/tools/hostcheck/tests.cpp"
    "$SRC_DIR/world/noise.cpp"
    "$SRC_DIR/world/terrain.cpp"
    "$SRC_DIR/world/biome.cpp"
    "$SRC_DIR/world/block.cpp"
    "$SRC_DIR/world/chunk.cpp"
    "$SRC_DIR/world/features.cpp"
    "$SRC_DIR/core/crashlog.cpp"
    "$SRC_DIR/core/job_system.cpp"
    "$SRC_DIR/mobs/mob_def.cpp"
    "$SRC_DIR/render/astc.cpp"
)
if ! "$CXX" -std=c++20 -O1 -g0 \
        -D__ANDROID__ -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DENTT_NO_ETO -DHOSTCHECK=1 \
        -I "$SRC_DIR" -I "$PROJ/tools/hostcheck/include" \
        -isystem "$TP/glm" -isystem "$TP/entt/include" -isystem "$TP/Vulkan-Headers/include" \
        -o "$OUT/tests" "${TEST_SRCS[@]}" "$OUT/obj/_stubs.o" -lpthread -ldl \
        2> "$OUT/tests-build.err"; then
    echo "✗ Тесты не собрались:"
    head -30 "$OUT/tests-build.err" | sed 's/^/    /'
    exit 1
fi

if ! "$OUT/tests"; then
    echo "✗ Тесты не прошли"
    exit 1
fi
