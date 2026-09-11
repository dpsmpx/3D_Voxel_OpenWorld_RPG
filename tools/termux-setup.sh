#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# Подготовка Termux к сборке VoxelRPG.
#
#   ./tools/termux-setup.sh                          # всё автоматически
#   ./tools/termux-setup.sh --ndk ~/android-ndk.zip  # NDK из скачанного архива
#   ./tools/termux-setup.sh --ndk https://...zip     # NDK по прямой ссылке
#   ./tools/termux-setup.sh --ndk ~/android-ndk-r27  # NDK уже распакован
#   ./tools/termux-setup.sh --skip-packages          # только NDK/SDK
#
# Что делает:
#   1. Ставит из pkg то, что там действительно есть.
#   2. Скачивает Android NDK для aarch64 — в pkg его НЕТ.
#   3. Скачивает Android SDK build-tools, если их нет в pkg.
#   4. Пишет переменные окружения в ~/.voxelrpg-env.
#
# Почему не «pkg install termux-ndk»: такого пакета не существует.
# Официальный NDK от Google собран под x86_64 и на телефоне не
# запустится. Рабочие сборки под aarch64 публикует проект
# lzhiyong/termux-ndk отдельными релизами на GitHub.
#
# Скрипт не молчит об ошибках: если NDK поставить не удалось, он
# говорит об этом прямо и НЕ пишет ANDROID_NDK_HOME в окружение,
# чтобы переменная не указывала в никуда.
# ============================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
HOME_DIR="${HOME:-/data/data/com.termux/files/home}"
ENV_FILE="$HOME_DIR/.voxelrpg-env"
TOOLS_DIR="$HOME_DIR/android"
NDK_REPO="lzhiyong/termux-ndk"

C_G='\033[0;32m'; C_Y='\033[1;33m'; C_R='\033[0;31m'; C_B='\033[0;34m'; C_0='\033[0m'
log()  { printf "${C_B}==>${C_0} %s\n" "$*"; }
ok()   { printf "${C_G}✓${C_0}  %s\n" "$*"; }
warn() { printf "${C_Y}!${C_0}  %s\n" "$*"; }
err()  { printf "${C_R}✗${C_0}  %s\n" "$*" >&2; }

NDK_SOURCE=""
SKIP_PACKAGES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --ndk)  NDK_SOURCE="${2:-}"; shift 2 || true ;;
        --ndk=*) NDK_SOURCE="${1#--ndk=}"; shift ;;
        --skip-packages) SKIP_PACKAGES=1; shift ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) err "неизвестный аргумент: $1"; exit 2 ;;
    esac
done

FAILED=0
NDK_OK=0
SDK_OK=0

# ------------------------------------------------------------
# 1. Пакеты из репозитория Termux
# ------------------------------------------------------------

# Ставит первый пакет из списка, который реально существует.
# Имена в Termux меняются между версиями (openjdk-17 -> openjdk-21),
# поэтому проверяем, а не надеемся.
install_any() {
    local label="$1"; shift
    for p in "$@"; do
        if apt-cache show "$p" >/dev/null 2>&1; then
            if pkg install -y "$p" >/dev/null 2>&1; then
                ok "$label: установлен $p"
                return 0
            fi
        fi
    done
    err "$label: ни один из вариантов не доступен: $*"
    echo "    Посмотрите, что есть: pkg search ${1%%-*}"
    return 1
}

if [ "$SKIP_PACKAGES" -eq 0 ]; then
    log "Обновление списка пакетов..."
    pkg update -y >/dev/null 2>&1 || warn "pkg update завершился с ошибкой, продолжаю"

    install_any "компилятор"     clang           || FAILED=1
    install_any "система сборки" cmake           || FAILED=1
    install_any "ninja"          ninja           || FAILED=1
    install_any "git"            git             || FAILED=1
    install_any "make"           make            || FAILED=1
    install_any "zip"            zip             || FAILED=1
    install_any "unzip"          unzip           || FAILED=1
    install_any "tar"            tar             || warn "tar не установился — архивы .tar.xz распаковать не выйдет"
    # curl и wget нужны для загрузки NDK: без них шаг 2 не сработает.
    install_any "curl"           curl            || FAILED=1
    install_any "wget"           wget            || FAILED=1
    install_any "zlib"           zlib            || FAILED=1

    # Компилятор шейдеров. В Termux он может называться по-разному.
    install_any "компилятор шейдеров" shaderc glslc glslang || {
        warn "glslc не установлен — шейдеры придётся компилировать вручную"
    }

    # Java. Имя пакета менялось: сейчас обычно openjdk-21.
    install_any "Java (JDK)" openjdk-21 openjdk-17 openjdk || FAILED=1

    # Инструменты упаковки APK. Часть живёт в основном репозитории,
    # часть — в TUR (Termux User Repository).
    if ! apt-cache show aapt2 >/dev/null 2>&1; then
        log "aapt2 нет в основном репозитории, подключаю TUR..."
        pkg install -y tur-repo >/dev/null 2>&1 || warn "tur-repo не установился"
        pkg update -y >/dev/null 2>&1 || true
    fi
    install_any "aapt2"     aapt2            || FAILED=1
    install_any "apksigner" apksigner        || FAILED=1
    install_any "d8"        d8 dx            || warn "d8 не найден — для чисто нативного APK он не обязателен"
else
    log "Установка пакетов пропущена (--skip-packages)"
fi

# ------------------------------------------------------------
# 2. Android NDK под aarch64
# ------------------------------------------------------------
mkdir -p "$TOOLS_DIR"
NDK_DIR="$TOOLS_DIR/android-ndk"

# --- сеть -----------------------------------------------------
# curl и wget взаимозаменяемы; берём тот, который есть.
http_get() {                      # url -> stdout
    local url="$1"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --max-time 60 -H 'Accept: application/vnd.github+json' "$url" && return 0
    fi
    if command -v wget >/dev/null 2>&1; then
        wget -q -T 60 -O - "$url" && return 0
    fi
    return 1
}

http_download() {                 # url dest
    local url="$1" dest="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --progress-bar --retry 3 --retry-delay 2 -o "$dest" "$url" && return 0
        rm -f "$dest"
    fi
    if command -v wget >/dev/null 2>&1; then
        wget --show-progress -q -T 60 -t 3 -O "$dest" "$url" && return 0
        rm -f "$dest"
    fi
    return 1
}

# Возвращает все ссылки на файлы релизов репозитория, свежие сверху.
# Основной путь — GitHub API. Он часто отвечает 403 «rate limit
# exceeded» с мобильного IP, поэтому есть запасной: атом-лента с
# тегами плюс HTML-фрагмент со списком файлов каждого релиза.
release_assets() {                # repo -> список url
    local repo="$1" body tags t
    body="$(http_get "https://api.github.com/repos/$repo/releases?per_page=30")" || body=""

    if printf '%s' "$body" | grep -q 'rate limit exceeded'; then
        warn "GitHub API: превышен лимит запросов, пробую запасной способ" >&2
        body=""
    fi

    if printf '%s' "$body" | grep -q 'browser_download_url'; then
        printf '%s' "$body" \
            | grep -oE '"browser_download_url": *"[^"]+"' \
            | sed 's/.*": *"//;s/"$//'
        return 0
    fi

    tags="$(http_get "https://github.com/$repo/releases.atom" 2>/dev/null \
            | grep -oE 'releases/tag/[^"<]+' | sed 's|releases/tag/||' | head -15)"
    [ -n "$tags" ] || return 1
    for t in $tags; do
        http_get "https://github.com/$repo/releases/expanded_assets/$t" 2>/dev/null \
            | grep -oE "/$repo/releases/download/[^\"]+" \
            | sed 's|^|https://github.com|'
    done
}

# Отбирает ссылки по ИМЕНИ файла, а не по всему URL: в ссылке есть
# ещё и тег релиза (.../download/ndk-r27/...), из-за которого шаблон
# «ndk» матчил бы и android-sdk-tools-aarch64.zip.
select_by_name() {                # regex; url читаются со stdin
    local re="$1" u n
    while IFS= read -r u; do
        [ -n "$u" ] || continue
        n="${u##*/}"
        if printf '%s' "$n" | grep -qiE "$re"; then printf '%s\n' "$u"; fi
    done
}

# NDK установлен полностью? Наличия каталога мало: архив мог
# распаковаться частично, а без toolchain.cmake и native_app_glue
# сборка упадёт уже на конфигурации CMake.
ndk_is_complete() {               # dir
    local d="$1" p clang=""
    [ -d "$d" ] || return 1
    for p in "$d"/toolchains/llvm/prebuilt/*/bin/clang; do
        [ -x "$p" ] && { clang="$p"; break; }
    done
    [ -n "$clang" ] || return 1
    [ -f "$d/build/cmake/android.toolchain.cmake" ] || return 1
    [ -f "$d/sources/android/native_app_glue/android_native_app_glue.c" ] || return 1
    return 0
}

extract_archive() {               # file destdir
    local f="$1" d="$2"
    mkdir -p "$d"
    case "$f" in
        *.zip)          unzip -q -o "$f" -d "$d" ;;
        *.tar.xz|*.txz) tar -xJf "$f" -C "$d" ;;
        *.tar.gz|*.tgz) tar -xzf "$f" -C "$d" ;;
        *.tar.bz2)      tar -xjf "$f" -C "$d" ;;
        *.7z)           command -v 7z >/dev/null 2>&1 || {
                            err "для .7z нужен p7zip: pkg install p7zip"; return 1; }
                        7z x -y -o"$d" "$f" >/dev/null ;;
        *)              err "неизвестный формат архива: $(basename "$f")"; return 1 ;;
    esac
}

# Каталог внутри архива называется по-разному (android-ndk-r27,
# ndk, иногда файлы лежат прямо в корне) — ищем по содержимому.
find_ndk_root() {                 # unpackdir
    local u="$1" c
    [ -d "$u/toolchains" ] && { printf '%s' "$u"; return 0; }
    for c in "$u"/*; do
        [ -d "$c/toolchains" ] && { printf '%s' "$c"; return 0; }
    done
    return 1
}

install_ndk_archive() {           # archive
    local arc="$1"
    local unpack="$TOOLS_DIR/.ndk-unpack"
    rm -rf "$unpack"
    log "Распаковка $(basename "$arc")..."
    extract_archive "$arc" "$unpack" || { rm -rf "$unpack"; return 1; }
    local root
    root="$(find_ndk_root "$unpack")" || {
        err "в архиве нет каталога NDK (не найден toolchains/)"
        rm -rf "$unpack"; return 1
    }
    rm -rf "$NDK_DIR"
    mv "$root" "$NDK_DIR"
    rm -rf "$unpack"
    ndk_is_complete "$NDK_DIR" || {
        err "распакованный NDK неполный: нет clang, android.toolchain.cmake или native_app_glue"
        echo "    Каталог оставлен для разбора: $NDK_DIR"
        echo "    Если архив скачался не полностью — удалите его и скачайте заново."
        return 1
    }
    return 0
}

ndk_manual_hint() {
    echo ""
    echo "  Android NDK для aarch64 в pkg отсутствует. Поставьте вручную:"
    echo ""
    echo "    1. Откройте https://github.com/$NDK_REPO/releases"
    echo "    2. Скачайте архив android-ndk-*-aarch64.zip (около 1 ГБ)"
    echo "    3. Запустите:"
    echo "         ./tools/termux-setup.sh --ndk ~/storage/downloads/android-ndk-....zip"
    echo ""
    echo "  Либо, если архив уже распакован:"
    echo "         ./tools/termux-setup.sh --ndk ~/android-ndk-r27"
    echo ""
}

if ndk_is_complete "$NDK_DIR" && [ -z "$NDK_SOURCE" ]; then
    ok "NDK уже установлен: $NDK_DIR"
    NDK_OK=1
elif [ -n "$NDK_SOURCE" ]; then
    # --- NDK указан вручную ---
    case "$NDK_SOURCE" in
        http://*|https://*)
            log "Скачиваю NDK по ссылке..."
            if http_download "$NDK_SOURCE" "$TOOLS_DIR/ndk-download"; then
                # Имя файла важно: по расширению выбирается распаковщик.
                ext="${NDK_SOURCE##*/}"
                mv "$TOOLS_DIR/ndk-download" "$TOOLS_DIR/$ext"
                install_ndk_archive "$TOOLS_DIR/$ext" && NDK_OK=1
                rm -f "$TOOLS_DIR/$ext"
            else
                err "не удалось скачать $NDK_SOURCE"
            fi
            ;;
        *)
            if [ -d "$NDK_SOURCE" ]; then
                NDK_SOURCE="$(cd "$NDK_SOURCE" && pwd)"
                if ndk_is_complete "$NDK_SOURCE"; then
                    NDK_DIR="$NDK_SOURCE"
                    ok "Использую готовый NDK: $NDK_DIR"
                    NDK_OK=1
                else
                    err "$NDK_SOURCE не похож на распакованный NDK"
                    echo "    Ожидались toolchains/llvm/prebuilt/*/bin/clang,"
                    echo "    build/cmake/android.toolchain.cmake и sources/android/native_app_glue/"
                fi
            elif [ -f "$NDK_SOURCE" ]; then
                install_ndk_archive "$NDK_SOURCE" && NDK_OK=1
            else
                err "путь не найден: $NDK_SOURCE"
            fi
            ;;
    esac
    [ "$NDK_OK" -eq 1 ] || FAILED=1
else
    # --- автоматический поиск ---
    if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
        err "нет ни curl, ни wget — скачать NDK нечем"
        echo "    pkg install curl wget"
        FAILED=1
    else
        log "Ищу Android NDK для aarch64 в релизах $NDK_REPO..."
        ASSETS="$(release_assets "$NDK_REPO")"

        if [ -z "$ASSETS" ]; then
            err "GitHub не ответил списком релизов (нет сети, прокси или лимит запросов)."
            ndk_manual_hint
            FAILED=1
        else
            # Сначала явно aarch64/arm64, потом любой архив с ndk в имени.
            NDK_URL="$(printf '%s\n' "$ASSETS" \
                | select_by_name 'ndk.*(aarch64|arm64).*\.(zip|tar\.xz|tar\.gz|7z)$' | head -1)"
            [ -n "$NDK_URL" ] || NDK_URL="$(printf '%s\n' "$ASSETS" \
                | select_by_name 'ndk.*\.(zip|tar\.xz|tar\.gz|7z)$' | head -1)"

            if [ -z "$NDK_URL" ]; then
                err "в релизах не нашлось архива NDK."
                echo "    Найденные файлы:"
                printf '%s\n' "$ASSETS" | head -10 | sed 's|^|      |'
                ndk_manual_hint
                FAILED=1
            else
                ARC="$TOOLS_DIR/$(basename "$NDK_URL")"
                log "Скачиваю $(basename "$NDK_URL") — около гигабайта, это надолго..."
                if http_download "$NDK_URL" "$ARC"; then
                    install_ndk_archive "$ARC" && NDK_OK=1
                    rm -f "$ARC"
                else
                    err "не удалось скачать $NDK_URL"
                    ndk_manual_hint
                fi
                [ "$NDK_OK" -eq 1 ] || FAILED=1
            fi
        fi
    fi
fi

# ------------------------------------------------------------
# 3. Android SDK build-tools под aarch64
#
# Нужны только для ручной упаковки APK через build.sh. Если в pkg
# уже есть aapt2/apksigner, этого достаточно и SDK можно пропустить.
# ------------------------------------------------------------
SDK_DIR="$TOOLS_DIR/android-sdk"
if command -v aapt2 >/dev/null 2>&1 && command -v apksigner >/dev/null 2>&1; then
    ok "aapt2 и apksigner есть в PATH, отдельный SDK не нужен"
elif [ -d "$SDK_DIR/build-tools" ]; then
    ok "SDK уже установлен: $SDK_DIR"
    SDK_OK=1
else
    log "Ищу Android SDK build-tools для aarch64..."
    SDK_URL="$(release_assets "$NDK_REPO" 2>/dev/null \
        | select_by_name 'sdk.*\.(zip|tar\.xz|tar\.gz)$' | head -1)"
    if [ -z "$SDK_URL" ]; then
        warn "SDK автоматически не найден — см. https://github.com/$NDK_REPO/releases"
        warn "Без него доступна только сборка .so; APK придётся паковать иначе"
    else
        SDK_ARC="$TOOLS_DIR/$(basename "$SDK_URL")"
        log "Скачиваю $(basename "$SDK_URL")..."
        if http_download "$SDK_URL" "$SDK_ARC"; then
            rm -rf "$TOOLS_DIR/.sdk-unpack"
            if extract_archive "$SDK_ARC" "$TOOLS_DIR/.sdk-unpack"; then
                rm -rf "$SDK_DIR"
                SDK_ROOT="$(find "$TOOLS_DIR/.sdk-unpack" -maxdepth 2 -type d -name 'build-tools' \
                            | head -1)"
                if [ -n "$SDK_ROOT" ]; then
                    mv "$(dirname "$SDK_ROOT")" "$SDK_DIR"
                    SDK_OK=1
                    ok "SDK установлен: $SDK_DIR"
                else
                    warn "в архиве SDK нет каталога build-tools — пропускаю"
                fi
            fi
            rm -rf "$TOOLS_DIR/.sdk-unpack"
            rm -f "$SDK_ARC"
        else
            warn "SDK не скачался — продолжаю без него"
        fi
    fi
fi

# ------------------------------------------------------------
# 4. Переменные окружения
#
# ANDROID_NDK_HOME пишем только тогда, когда NDK действительно
# на месте. Переменная, указывающая в никуда, хуже её отсутствия:
# CMake падает с невнятной ошибкой вместо понятного «NDK не найден».
# ------------------------------------------------------------
log "Пишу $ENV_FILE..."
{
    echo "# Окружение сборки VoxelRPG. Создано tools/termux-setup.sh"
    if [ "$NDK_OK" -eq 1 ]; then
        echo "export ANDROID_NDK_HOME=\"$NDK_DIR\""
        echo "export ANDROID_NDK=\"\$ANDROID_NDK_HOME\""
        for pb in "$NDK_DIR"/toolchains/llvm/prebuilt/*/bin; do
            [ -d "$pb" ] && { echo "export PATH=\"$pb:\$PATH\""; break; }
        done
    else
        echo "# NDK не установлен — ANDROID_NDK_HOME намеренно не задан."
        echo "# Поставьте его: ./tools/termux-setup.sh --ndk <архив или каталог>"
    fi
    if [ "$SDK_OK" -eq 1 ]; then
        echo "export ANDROID_HOME=\"$SDK_DIR\""
        echo "export ANDROID_SDK_ROOT=\"\$ANDROID_HOME\""
        echo "export PATH=\"\$ANDROID_HOME/build-tools:\$PATH\""
    fi
} > "$ENV_FILE"
ok "Переменные записаны"

# Подключаем автоматически при следующем запуске оболочки.
BASHRC="$HOME_DIR/.bashrc"
if ! grep -q 'voxelrpg-env' "$BASHRC" 2>/dev/null; then
    echo "[ -f \"$ENV_FILE\" ] && . \"$ENV_FILE\"" >> "$BASHRC"
    ok "Добавлено в ~/.bashrc"
fi

echo ""
if [ "$FAILED" -eq 0 ] && [ "$NDK_OK" -eq 1 ]; then
    ok "==========================================="
    ok "  Готово. Дальше:"
    ok "      source $ENV_FILE"
    ok "      ./build.sh"
    ok "==========================================="
else
    warn "==========================================="
    warn "  Часть шагов не прошла — см. сообщения выше."
    [ "$NDK_OK" -eq 1 ] || warn "  Главное: NDK не установлен, нативная сборка невозможна."
    warn "  Проверить состояние: ./tools/termux-doctor.sh"
    warn "==========================================="
    exit 1
fi
