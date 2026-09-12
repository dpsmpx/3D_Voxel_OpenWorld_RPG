#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# Собирает кодировщик ASTC от ARM прямо в Termux.
#
#   ./tools/build-astcenc.sh
#
# Зачем отдельный скрипт:
#
#   1. В репозиториях Termux astcenc нет ни под одним именем —
#      pkg его не поставит.
#   2. Готовые сборки ARM под Linux/aarch64 в Termux не запускаются:
#      они рассчитаны на glibc, а здесь Bionic, и падают на
#      выравнивании TLS.
#   3. Сборка из исходников спотыкается о NEON-макросы в stb_image,
#      который лежит внутри astcenccli_image_external.cpp. Лечится
#      снятием __ARM_NEON именно для этого файла.
#
# Результат кладётся в tools/astcenc/ — оттуда его находит build.sh.
# ============================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
. "$PROJ/tools/ndk-common.sh"

VERSION="${ASTCENC_VERSION:-5.6.0}"
WORK="${ASTCENC_WORKDIR:-$HOME/.voxelrpg/astc-build}"
DEST="$PROJ/tools/astcenc"

C_G='\033[0;32m'; C_Y='\033[1;33m'; C_R='\033[0;31m'; C_B='\033[0;34m'; C_0='\033[0m'
log()  { printf "${C_B}==>${C_0} %s\n" "$*"; }
ok()   { printf "${C_G}✓${C_0}  %s\n" "$*"; }
warn() { printf "${C_Y}!${C_0}  %s\n" "$*"; }
err()  { printf "${C_R}✗${C_0}  %s\n" "$*" >&2; }

if EXISTING="$(find_astcenc)"; then
    ok "Кодировщик уже есть: $EXISTING"
    echo "    Пересобрать принудительно: rm -rf $DEST $WORK && $0"
    exit 0
fi

for tool in cmake ninja make; do
    command -v "$tool" >/dev/null 2>&1 && break
done
command -v cmake >/dev/null 2>&1 || { err "нужен cmake: pkg install cmake"; exit 1; }
if ! HOST_CXX="$(host_cxx)"; then
    err "нужен рабочий компилятор: pkg install clang"
    exit 1
fi

mkdir -p "$WORK"
SRC="$WORK/astc-encoder-$VERSION"

if [ ! -d "$SRC" ]; then
    ARC="$WORK/astc-encoder-$VERSION.tar.gz"
    URL="https://github.com/ARM-software/astc-encoder/archive/refs/tags/$VERSION.tar.gz"
    log "Скачиваю исходники $VERSION..."
    if ! http_download "$URL" "$ARC"; then
        err "не удалось скачать $URL"
        err "Скачайте архив вручную и распакуйте в $SRC"
        exit 1
    fi
    log "Распаковка..."
    extract_archive "$ARC" "$WORK" || exit 1
    rm -f "$ARC"
    [ -d "$SRC" ] || {
        SRC="$(find "$WORK" -maxdepth 1 -type d -name 'astc-encoder*' | head -1)"
    }
    [ -d "$SRC" ] || { err "каталог с исходниками не найден"; exit 1; }
fi

# --- обход NEON-макросов в stb_image ---
# astcenccli_image_external.cpp включает stb_image, а тот на Bionic
# спотыкается о собственные NEON-пути. Компилируем именно этот файл
# без __ARM_NEON — на скорость кодирования атласа это не влияет,
# затрагивается только чтение PNG/TGA.
CORE="$SRC/Source/cmake_core.cmake"
if [ -f "$CORE" ] && ! grep -q 'U__ARM_NEON' "$CORE"; then
    log "Правлю $CORE (NEON в stb_image)..."
    cp "$CORE" "$CORE.orig"
    cat >> "$CORE" <<'PATCH'

# Добавлено tools/build-astcenc.sh: stb_image внутри
# astcenccli_image_external.cpp не собирается с NEON на Bionic.
if(CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set_source_files_properties(astcenccli_image_external.cpp
        PROPERTIES COMPILE_FLAGS "-U__ARM_NEON -U__ARM_NEON__")
endif()
PATCH
    ok "Правка внесена, оригинал сохранён как cmake_core.cmake.orig"
fi

BUILD="$SRC/build-termux"
log "Конфигурация..."
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)

# ISA_NONE — универсальный вариант без векторных расширений. Атлас
# кодируется один раз за сборку, скорость роли не играет, а сборки с
# NEON в Termux ломаются.
if ! cmake -S "$SRC" -B "$BUILD" "${GEN[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DASTCENC_ISA_NONE=ON \
        -DASTCENC_CLI=ON \
        -DCMAKE_CXX_COMPILER="$HOST_CXX" > "$WORK/configure.log" 2>&1; then
    err "cmake не сконфигурировал сборку:"
    tail -20 "$WORK/configure.log" | sed 's/^/    /'
    exit 1
fi

log "Сборка (это займёт несколько минут)..."
if ! cmake --build "$BUILD" > "$WORK/build.log" 2>&1; then
    err "сборка не прошла:"
    tail -25 "$WORK/build.log" | sed 's/^/    /'
    exit 1
fi

BIN="$(find "$BUILD" -type f -name 'astcenc-*' -perm -u+x | head -1)"
if [ -z "$BIN" ]; then
    err "исполняемый файл не найден в $BUILD"
    exit 1
fi

mkdir -p "$DEST"
cp "$BIN" "$DEST/"
chmod 755 "$DEST/$(basename "$BIN")"

if RESULT="$(find_astcenc)"; then
    ok "Готово: $RESULT"
    echo ""
    echo "  Теперь ./build.sh соберёт атлас в ASTC 4x4 — вчетверо"
    echo "  меньше видеопамяти под текстуры."
else
    err "собранный файл не запускается — см. $WORK/build.log"
    exit 1
fi
