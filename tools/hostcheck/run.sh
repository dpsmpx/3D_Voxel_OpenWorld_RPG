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

# grep -c печатает «0» и выходит с кодом 1, когда совпадений нет.
# Связка «|| echo 0» дописывала к этому нулю ещё один, счётчик
# становился двухстрочным, и сравнение ниже падало с «integer
# expression expected» — то есть проверка предупреждений молча не
# работала. «|| true» оставляет ровно то, что напечатал grep.
ERRORS=$( { grep -c "error:" "$LOG" || true; } 2>/dev/null )
WARNS=$( { grep -c "warning:" "$LOG" || true; } 2>/dev/null )
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
. "$PROJ/tools/glsl-cc.sh"
if glsl_find; then
    echo "==> Компиляция шейдеров ($GLSL_CC)..."
    mkdir -p "$OUT/shaders"
    SH_FAIL=0
    for f in "$CPP_DIR"/shaders/*.vert "$CPP_DIR"/shaders/*.frag; do
        [ -f "$f" ] || continue
        # Только stderr: glslangValidator печатает имя файла в stdout
        # и при успехе тоже, а здесь всякая строка считается дефектом.
        if ! glsl_compile "$f" "$OUT/shaders/$(basename "$f").spv" \
                >/dev/null 2>> "$OUT/shaders.err"; then
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
    echo "! шейдеры не проверены:"
    glsl_hint | sed 's/^/  /'
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

# ---- дескрипторы у конвейеров ----
echo "==> Дескрипторы конвейеров..."
if ! python3 "$PROJ/tools/hostcheck/check_bindings.py"; then
    exit 1
fi

# ---- таблицы строк ----
# static_assert ловит забытую строку, но не строку, вставленную не на
# своё место: количество сходится, а все подписи ниже — чужие.
echo "==> Таблицы строк..."
if ! python3 "$PROJ/tools/hostcheck/check_localization.py"; then
    exit 1
fi

# ---- обход граней ----
# Половина каждого куба мобов и NPC просвечивала насквозь: три грани из
# шести были намотаны наоборот. Компилятору такая таблица безразлична.
echo "==> Обход граней..."
if ! python3 "$PROJ/tools/hostcheck/check_winding.py"; then
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
#
# Линкуемся с теми же объектными файлами, которые уже собраны выше для
# libnative-lib. Раньше здесь жил список исходников, который
# приходилось дополнять вручную каждый раз, когда проверка задевала
# новую часть игры: связи тянутся далеко (диалог -> квесты ->
# прогрессия -> предметы), и список дорос до четырёх десятков строк,
# а его пополнение выглядело как ошибка линковки, а не как задача.
#
# Конфликта точек входа нет: игра начинается с android_main, а main
# есть только в самих тестах.
echo "==> Тесты..."
if ! "$CXX" -std=c++20 -O1 -g0 \
        -D__ANDROID__ -DVK_USE_PLATFORM_ANDROID_KHR \
        -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DENTT_NO_ETO -DHOSTCHECK=1 \
        -I "$SRC_DIR" -I "$PROJ/tools/hostcheck/include" \
        -isystem "$TP/glm" -isystem "$TP/entt/include" -isystem "$TP/Vulkan-Headers/include" \
        -o "$OUT/tests" "$PROJ/tools/hostcheck/tests.cpp" "$OUT"/obj/*.o \
        -lz -lpthread -ldl \
        2> "$OUT/tests-build.err"; then
    echo "✗ Тесты не собрались:"
    head -30 "$OUT/tests-build.err" | sed 's/^/    /'
    exit 1
fi

if ! "$OUT/tests"; then
    echo "✗ Тесты не прошли"
    exit 1
fi

# ---- процедурные реки ----
# Компилируем отдельный гидрологический smoke-test поверх тех же
# объектных файлов, что идут в линковку native-lib.
echo "==> Проверка генератора рек..."
if ! "$CXX" -std=c++20 -O1 -g0 \
        -D__ANDROID__ -DVK_USE_PLATFORM_ANDROID_KHR \
        -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DENTT_NO_ETO -DHOSTCHECK=1 \
        -I "$SRC_DIR" -I "$PROJ/tools/hostcheck/include" \
        -isystem "$TP/glm" -isystem "$TP/entt/include" -isystem "$TP/Vulkan-Headers/include" \
        -o "$OUT/rivercheck" "$PROJ/tools/rivercheck/rivercheck.cpp" "$OUT"/obj/*.o \
        -lz -lpthread -ldl \
        2> "$OUT/rivercheck-build.err"; then
    echo "✗ rivercheck не собрался:"
    head -30 "$OUT/rivercheck-build.err" | sed 's/^/    /'
    exit 1
fi

if ! "$OUT/rivercheck"; then
    echo "✗ Генератор рек не прошёл проверку"
    exit 1
fi

# ---- списки исходников у инструментов ----
#
# bench, vkcheck и isocheck собирают игру из СВОИХ списков файлов, а
# CI их не запускает (Vulkan есть не везде). Когда появился
# world/hydrology.cpp, все три перестали линковаться — и этого никто
# не видел. Здесь каждый список линкуется (без запуска) с уже
# собранными объектами и заглушками: забытый файл — ошибка сразу.
echo "==> Списки исходников инструментов..."
mkdir -p "$OUT/tools"
for TOOL in bench vkcheck isocheck; do
    SCRIPT="$PROJ/tools/$TOOL/run.sh"
    MAIN="$PROJ/tools/$TOOL/$TOOL.cpp"
    [ -f "$SCRIPT" ] && [ -f "$MAIN" ] || continue
    TOOL_OBJS=()
    # "$SRC"/dir/{a,b}.cpp и "$SRC"/dir/a.cpp — ровно как в скрипте;
    # фигурные скобки раскрывает сам bash. Свои заглушки Android у
    # инструментов свои, поэтому повтор определения разрешён: ищем здесь
    # только НЕРАЗРЕШЁННЫЕ символы, и побеждает определение инструмента.
    while read -r SPEC; do
        for F in $(eval echo "$SPEC"); do
            TOOL_OBJS+=("$OUT/obj/src_$(echo "${F%.cpp}" | tr '/' '_').o")
        done
    done < <(grep -o '"\$SRC"/[A-Za-z0-9_/{},.-]*\.cpp' "$SCRIPT" | sed 's|"\$SRC"/||')
    for O in "${TOOL_OBJS[@]}"; do
        [ -f "$O" ] || { echo "✗ $TOOL: в списке файл, которого нет: $O"; exit 1; }
    done
    if ! "$CXX" -std=c++20 -O0 -g0 \
            -D__ANDROID__ -DVK_USE_PLATFORM_ANDROID_KHR \
            -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DENTT_NO_ETO -DHOSTCHECK=1 \
            -I "$SRC_DIR" -I "$PROJ/tools/hostcheck/include" \
            -isystem "$TP/glm" -isystem "$TP/entt/include" -isystem "$TP/Vulkan-Headers/include" \
            -o "$OUT/tools/$TOOL" "$MAIN" "${TOOL_OBJS[@]}" "$OUT/obj/_stubs.o" \
            -Wl,--allow-multiple-definition \
            -lz -lpthread -ldl 2> "$OUT/tools/$TOOL.err"; then
        echo "✗ $TOOL не линкуется со своим списком исходников (tools/$TOOL/run.sh):"
        grep -E "undefined|error" "$OUT/tools/$TOOL.err" | sed 's/^/    /' | head -20
        exit 1
    fi
done
# soak и uishot берут все объекты разом — у них ломается не список, а
# вызовы: SaveManager::save/load получили жителей и клады, и soak
# перестал собираться, никем не замеченный.
for TOOL in soak uishot; do
    MAIN="$PROJ/tools/$TOOL/$TOOL.cpp"
    [ -f "$MAIN" ] || continue
    if ! "$CXX" -std=c++20 -O0 -g0 \
            -D__ANDROID__ -DVK_USE_PLATFORM_ANDROID_KHR \
            -DGLM_FORCE_DEPTH_ZERO_TO_ONE -DGLM_ENABLE_EXPERIMENTAL -DENTT_NO_ETO -DHOSTCHECK=1 \
            -I "$SRC_DIR" -I "$PROJ/tools/hostcheck/include" \
            -isystem "$TP/glm" -isystem "$TP/entt/include" -isystem "$TP/Vulkan-Headers/include" \
            -o "$OUT/tools/$TOOL" "$MAIN" "$OUT"/obj/*.o \
            -lz -lpthread -ldl 2> "$OUT/tools/$TOOL.err"; then
        echo "✗ $TOOL не собирается с текущим кодом игры:"
        grep -E "error|undefined" "$OUT/tools/$TOOL.err" | sed 's/^/    /' | head -20
        exit 1
    fi
done
echo "✓ bench, vkcheck, isocheck, soak, uishot собираются"

# ---- две конфигурации сборки ----
#
# debug_scene включается флагом времени компиляции, значит проверить
# обе стороны в одном бинарнике нельзя: нужно собрать дважды.
echo "==> Конфигурации сборки..."
for DIAG in 0 1; do
    DEF=""
    [ "$DIAG" = "1" ] && DEF="-DVOXEL_DEBUG_SCENE=1"
    if ! "$CXX" -std=c++20 -O0 -g0 $DEF \
            -D__ANDROID__ -DHOSTCHECK=1 \
            -I "$SRC_DIR" -I "$PROJ/tools/hostcheck/include" -isystem "$TP/glm" \
            -o "$OUT/diagcheck" \
            "$PROJ/tools/hostcheck/diag_build_check.cpp" \
            "$SRC_DIR/config/settings.cpp" "$SRC_DIR/core/crashlog.cpp" \
            "$SRC_DIR/core/shared_dir.cpp" \
            "$OUT/obj/_stubs.o" -lpthread -ldl \
            > "$OUT/diagcheck.err" 2>&1; then
        echo "✗ Проверка конфигураций не собралась:"
        head -20 "$OUT/diagcheck.err" | sed 's/^/    /'
        exit 1
    fi
    if ! "$OUT/diagcheck"; then
        echo "✗ Конфигурация сборки ведёт себя не так, как объявлено"
        exit 1
    fi
done

# ---- Java: активность с выбором файла ----
#
# Java в проекте ровно один класс, и до него не доставала ни одна
# проверка: собирался он только вместе с APK, то есть в CI и через
# двадцать минут. Опечатка в имени метода стоила бы целого прогона.
#
# Полного android.jar здесь нет — есть заглушки с подписями того, чего
# класс касается (tools/hostcheck/javastub). Этого хватает, чтобы
# поймать опечатку, лишнюю запятую и несовпадение типов.
JAVA_SRC="$PROJ/app/src/main/java"
if [ -d "$JAVA_SRC" ] && command -v javac >/dev/null 2>&1; then
    echo "==> Java активности..."
    JAVA_OUT="$OUT/javac"
    rm -rf "$JAVA_OUT"; mkdir -p "$JAVA_OUT"
    if ! javac -nowarn -d "$JAVA_OUT" \
            -sourcepath "$PROJ/tools/hostcheck/javastub:$JAVA_SRC" \
            $(find "$JAVA_SRC" -name '*.java') > "$OUT/javac.log" 2>&1; then
        echo "✗ Java не собралась:"
        grep -v JAVA_TOOL_OPTIONS "$OUT/javac.log" | head -20 | sed 's/^/    /'
        exit 1
    fi
    CLASSES=$(find "$JAVA_OUT" -name '*.class' | wc -l)
    echo "✓ Классов собрано: $CLASSES, ошибок нет"
elif [ -d "$JAVA_SRC" ]; then
    echo "==> Java активности: пропущено (нет javac)"
fi

# ---- кадр настоящим Vulkan ----
#
# Всё, что выше, проверяет данные и исходный текст. Ни одна из этих
# проверок не заметила, что конвейер объявляет лицевой не ту сторону
# грани: данные были верны, текст выглядел разумно, а мир на
# устройстве был виден изнутри. Заметить это может только настоящий
# Vulkan с настоящими шейдерами — им и заканчиваем.
#
# Драйвера может не быть (сборка на телефоне, чистый образ CI) — тогда
# шаг пропускается, а не валит проверку.
if [ -f /usr/share/vulkan/icd.d/lvp_icd.json ] || [ -n "${VK_ICD_FILENAMES:-}" ]; then
    echo "==> Кадр настоящим Vulkan..."
    if ! "$PROJ/tools/vkcheck/run.sh" --assert-solid > "$OUT/vkcheck.log" 2>&1; then
        echo "✗ Проверка графики не прошла:"
        grep -E "ПРОВАЛ|\[слой\]|error" "$OUT/vkcheck.log" | head -20 | sed 's/^/    /'
        echo "    полный лог: $OUT/vkcheck.log"
        exit 1
    fi
    grep -E "^vkcheck:" "$OUT/vkcheck.log" | sed 's/^/  /'

    # Второй кадр — с НЕБОМ и погодой.
    #
    # Первый рисует небо ровной заливкой: так считаются пиксели
    # геометрии. Из-за этого самый дорогой шейдер кадра — и
    # единственный, где живут тучи и радуга, — не выполнялся здесь ни
    # разу, и всё, что о нём было известно, это что он собрался.
    echo "==> Кадр с небом и погодой..."
    if ! "$PROJ/tools/vkcheck/run.sh" --sky 1 --time 0.30 --yaw 3.14 \
            --pitch 0.25 --cloud 0.75 --rain 0.6 --rainbow 1.0 \
            --wind 6 2 --out "$OUT/weather.ppm" \
            > "$OUT/vkweather.log" 2>&1; then
        echo "✗ Кадр с погодой не нарисовался:"
        grep -E "ПРОВАЛ|\[слой\]|error" "$OUT/vkweather.log" | head -20 | sed 's/^/    /'
        echo "    полный лог: $OUT/vkweather.log"
        exit 1
    fi
    grep -E "^vkcheck: (погода|кадр|пикселей)" "$OUT/vkweather.log" | sed 's/^/  /'

    # ---- Третий кадр: НОЧЬ и факел ----
    #
    # У блоков с самого начала были isEmissive и lightLevel, а читать
    # их было некому: ночью и в пещере фонарь светил ровно столько же,
    # сколько булыжник. Проверить «светит» можно только светом — то
    # есть двумя одинаковыми кадрами, в одном из которых факел есть.
    echo "==> Ночь: без факела и с факелом..."
    lumaOf() {   # $1 — лог кадра
        grep -oE "средняя яркость нижней половины кадра: [0-9.]+" "$1" \
            | grep -oE "[0-9.]+$"
    }
    VK_NIGHT_ARGS=(--time 0.0 --pitch -0.75 --height 3)
    if "$PROJ/tools/vkcheck/run.sh" "${VK_NIGHT_ARGS[@]}" \
            --out "$OUT/night.ppm" > "$OUT/vknight.log" 2>&1 &&
       "$PROJ/tools/vkcheck/run.sh" "${VK_NIGHT_ARGS[@]}" --torch 1.0 \
            --out "$OUT/torch.ppm" > "$OUT/vktorch.log" 2>&1
    then
        DARK=$(lumaOf "$OUT/vknight.log")
        LIT=$(lumaOf "$OUT/vktorch.log")
        grep -E "^vkcheck: факел" "$OUT/vktorch.log" | sed 's/^/  /'
        echo "  vkcheck: ночь без факела $DARK, с факелом $LIT"
        # Порог с запасом: замер даёт 0.213 против 0.353, то есть
        # +66%. Требуем хотя бы +20% — меньше значило бы, что свет
        # где-то по дороге потерялся.
        if ! awk -v a="$DARK" -v b="$LIT" 'BEGIN{exit !(b > a * 1.20)}'; then
            echo "✗ Факел не светит: ночью с ним не светлее, чем без него"
            exit 1
        fi
        echo "✓ Факел освещает ночь"
    else
        echo "✗ Ночные кадры не нарисовались:"
        tail -5 "$OUT/vknight.log" "$OUT/vktorch.log" 2>/dev/null | sed 's/^/    /'
        exit 1
    fi

    # ---- Четвёртый и пятый кадры: КРАПЧАТОСТЬ БЛОКОВ ----
    #
    # Утверждение «отклонение цвета зависит от зерна мира и только от
    # него» — про ПАРУ кадров, и по одному кадру его не проверить.
    # Здесь их три, и каждый закрывает свою половину:
    #
    #   один мир дважды      -> обязаны совпасть ДО ПИКСЕЛЯ;
    #   другое зерно         -> обязаны разойтись, но чуть-чуть.
    #
    # Рельеф во всех трёх один и тот же: зерно крапчатости разводится
    # с зерном мира отдельным ключом. Иначе «цвет стал другим» не
    # отличить от «мир стал другим».
    #
    # Кадр мелкий намеренно: считается он процессором, а решают здесь
    # отношения, а не разрешение.
    echo "==> Крапчатость блоков: тот же мир, другое зерно..."
    TINT_ARGS=(--size 420 260 --pos 33.6 -2.7 --yaw 1.2 --pitch -0.10)
    if "$PROJ/tools/vkcheck/run.sh" "${TINT_ARGS[@]}" --tintseed 1             --out "$OUT/tint_a.ppm" > "$OUT/vktint_a.log" 2>&1 &&
       "$PROJ/tools/vkcheck/run.sh" "${TINT_ARGS[@]}" --tintseed 1             --out "$OUT/tint_b.ppm" --diff "$OUT/tint_a.ppm"             > "$OUT/vktint_b.log" 2>&1 &&
       "$PROJ/tools/vkcheck/run.sh" "${TINT_ARGS[@]}" --tintseed 77             --out "$OUT/tint_c.ppm" --diff "$OUT/tint_a.ppm"             > "$OUT/vktint_c.log" 2>&1
    then
        diffOf() {   # $1 — лог кадра, $2 — номер поля
            grep -oE "отличается пикселей [0-9]+ из [0-9]+ \(([0-9.]+)\), среднее \|д\| [0-9.]+, наибольшее \|д\| [0-9]+" "$1" \
                | grep -oE "[0-9.]+" | sed -n "${2}p"
        }
        SAME=$(diffOf "$OUT/vktint_b.log" 3)
        OTHER=$(diffOf "$OUT/vktint_c.log" 3)
        MEAN=$(diffOf "$OUT/vktint_c.log" 4)
        MAX=$(diffOf "$OUT/vktint_c.log" 5)
        echo "  vkcheck: то же зерно — отличается $SAME кадра;" \
             "другое зерно — $OTHER, среднее |д| $MEAN, наибольшее $MAX"

        # 1. Один мир — один вид. Ни одного пикселя мимо.
        if ! awk -v a="$SAME" 'BEGIN{exit !(a == 0)}'; then
            echo "✗ Один и тот же мир нарисовался по-разному:"
            echo "  крапчатость обязана считаться из координат и зерна,"
            echo "  а не из чего-то, что меняется между кадрами"
            exit 1
        fi
        # 2. Другое зерно — другая крапчатость. Замер даёт 0.67 кадра;
        #    требуем хотя бы десятую часть — меньше значило бы, что
        #    зерно по дороге потерялось и красит одна константа.
        if ! awk -v a="$OTHER" 'BEGIN{exit !(a > 0.10)}'; then
            echo "✗ Зерно ни на что не влияет: два мира покрашены одинаково"
            exit 1
        fi
        # 3. И при этом НЕБОЛЬШАЯ. Замер даёт среднее 2.8 и предел 14
        #    из 255; потолок с запасом — 8 и 30. Выше — это уже не
        #    колебание оттенка, а другой цвет.
        if ! awk -v m="$MEAN" -v x="$MAX" 'BEGIN{exit !(m < 8.0 && x < 30)}'; then
            echo "✗ Крапчатость слишком крупная: среднее $MEAN, предел $MAX из 255"
            exit 1
        fi
        echo "✓ Крапчатость зависит от зерна, повторяется и невелика"
    else
        echo "✗ Кадры крапчатости не нарисовались:"
        tail -5 "$OUT"/vktint_*.log 2>/dev/null | sed 's/^/    /'
        exit 1
    fi
else
    echo "==> Кадр настоящим Vulkan: пропущено (нет программного драйвера;"
    echo "    apt-get install -y mesa-vulkan-drivers vulkan-validationlayers)"
fi
