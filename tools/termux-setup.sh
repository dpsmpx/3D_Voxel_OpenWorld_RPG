#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# Подготовка Termux к сборке VoxelRPG.
#
#   ./tools/termux-setup.sh
#
# Что делает:
#   1. Ставит из pkg то, что там действительно есть.
#   2. Скачивает Android NDK для aarch64 — в pkg его НЕТ.
#   3. Скачивает Android SDK build-tools для aarch64.
#   4. Пишет переменные окружения в ~/.voxelrpg-env.
#
# Почему не «pkg install termux-ndk»: такого пакета не существует.
# Официальный NDK от Google собран под x86_64 и на телефоне не
# запустится. Рабочие сборки под aarch64 публикует проект
# lzhiyong/termux-ndk отдельными релизами на GitHub.
# ============================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
HOME_DIR="${HOME:-/data/data/com.termux/files/home}"
ENV_FILE="$HOME_DIR/.voxelrpg-env"
TOOLS_DIR="$HOME_DIR/android"

C_G='\033[0;32m'; C_Y='\033[1;33m'; C_R='\033[0;31m'; C_B='\033[0;34m'; C_0='\033[0m'
log()  { printf "${C_B}==>${C_0} %s\n" "$*"; }
ok()   { printf "${C_G}✓${C_0}  %s\n" "$*"; }
warn() { printf "${C_Y}!${C_0}  %s\n" "$*"; }
err()  { printf "${C_R}✗${C_0}  %s\n" "$*" >&2; }

# ------------------------------------------------------------
# 1. Пакеты из репозитория Termux
# ------------------------------------------------------------
log "Обновление списка пакетов..."
pkg update -y >/dev/null 2>&1 || warn "pkg update завершился с ошибкой, продолжаю"

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

FAILED=0
install_any "компилятор"    clang            || FAILED=1
install_any "система сборки" cmake           || FAILED=1
install_any "ninja"         ninja            || FAILED=1
install_any "git"           git              || FAILED=1
install_any "make"          make             || FAILED=1
install_any "zip"           zip              || FAILED=1
install_any "unzip"         unzip            || FAILED=1
install_any "wget"          wget             || FAILED=1
install_any "zlib"          zlib             || FAILED=1

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

# ------------------------------------------------------------
# 2. Android NDK под aarch64
# ------------------------------------------------------------
mkdir -p "$TOOLS_DIR"

# Находит ссылку на архив NDK в последнем релизе через GitHub API,
# чтобы не зашивать имя файла: оно меняется от версии к версии.
fetch_release_asset() {
    local repo="$1" tag="$2" pattern="$3"
    local api="https://api.github.com/repos/$repo/releases"
    [ "$tag" != "latest" ] && api="$api/tags/$tag" || api="$api/latest"
    curl -sSL --max-time 60 "$api" 2>/dev/null \
        | grep -oE '"browser_download_url": *"[^"]+"' \
        | sed 's/.*": *"//;s/"$//' \
        | grep -E "$pattern" \
        | head -1
}

NDK_DIR="$TOOLS_DIR/android-ndk"
if [ -x "$NDK_DIR/toolchains/llvm/prebuilt/linux-aarch64/bin/clang" ]; then
    ok "NDK уже установлен: $NDK_DIR"
else
    log "Ищу Android NDK для aarch64..."
    NDK_URL="$(fetch_release_asset lzhiyong/termux-ndk latest 'android-ndk.*\.zip$')"

    if [ -z "$NDK_URL" ]; then
        err "Не удалось определить ссылку на NDK автоматически."
        echo ""
        echo "  В pkg Android NDK нет. Скачайте вручную со страницы релизов:"
        echo "      https://github.com/lzhiyong/termux-ndk/releases"
        echo "  Нужен архив android-ndk-*.zip для linux-aarch64, затем:"
        echo "      unzip android-ndk-*.zip -d $TOOLS_DIR"
        echo "      mv $TOOLS_DIR/android-ndk-* $NDK_DIR"
        echo ""
        FAILED=1
    else
        log "Скачиваю $(basename "$NDK_URL") — это несколько сотен мегабайт..."
        if wget -q --show-progress -O "$TOOLS_DIR/ndk.zip" "$NDK_URL"; then
            log "Распаковка..."
            unzip -q -o "$TOOLS_DIR/ndk.zip" -d "$TOOLS_DIR"
            rm -f "$TOOLS_DIR/ndk.zip"
            # Каталог внутри архива называется android-ndk-rNN.
            EXTRACTED="$(find "$TOOLS_DIR" -maxdepth 1 -type d -name 'android-ndk-*' | head -1)"
            if [ -n "$EXTRACTED" ]; then
                rm -rf "$NDK_DIR"
                mv "$EXTRACTED" "$NDK_DIR"
                ok "NDK установлен: $NDK_DIR"
            else
                err "В архиве нет каталога android-ndk-*"
                FAILED=1
            fi
        else
            err "Не удалось скачать NDK"
            FAILED=1
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
elif [ -d "$SDK_DIR" ]; then
    ok "SDK уже установлен: $SDK_DIR"
else
    log "Ищу Android SDK build-tools для aarch64..."
    SDK_URL="$(fetch_release_asset lzhiyong/termux-ndk android-sdk 'android-sdk.*\.zip$')"
    if [ -z "$SDK_URL" ]; then
        warn "SDK автоматически не найден — см. https://github.com/lzhiyong/termux-ndk/releases"
        warn "Без него доступна только сборка .so; APK придётся паковать иначе"
    else
        log "Скачиваю $(basename "$SDK_URL")..."
        if wget -q --show-progress -O "$TOOLS_DIR/sdk.zip" "$SDK_URL"; then
            unzip -q -o "$TOOLS_DIR/sdk.zip" -d "$TOOLS_DIR"
            rm -f "$TOOLS_DIR/sdk.zip"
            [ -d "$TOOLS_DIR/android-sdk" ] || \
                mv "$(find "$TOOLS_DIR" -maxdepth 1 -type d -name 'android-sdk*' | head -1)" "$SDK_DIR" 2>/dev/null
            ok "SDK установлен: $SDK_DIR"
        else
            warn "SDK не скачался — продолжаю без него"
        fi
    fi
fi

# ------------------------------------------------------------
# 4. Переменные окружения
# ------------------------------------------------------------
log "Пишу $ENV_FILE..."
{
    echo "# Окружение сборки VoxelRPG. Создано tools/termux-setup.sh"
    echo "export ANDROID_NDK_HOME=\"$NDK_DIR\""
    echo "export ANDROID_NDK=\"\$ANDROID_NDK_HOME\""
    [ -d "$SDK_DIR" ] && echo "export ANDROID_HOME=\"$SDK_DIR\""
    [ -d "$SDK_DIR" ] && echo "export ANDROID_SDK_ROOT=\"\$ANDROID_HOME\""
    echo "export PATH=\"\$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-aarch64/bin:\$PATH\""
    [ -d "$SDK_DIR/build-tools" ] && \
        echo "export PATH=\"\$ANDROID_HOME/build-tools:\$PATH\""
} > "$ENV_FILE"
ok "Переменные записаны"

# Подключаем автоматически при следующем запуске оболочки.
BASHRC="$HOME_DIR/.bashrc"
if ! grep -q 'voxelrpg-env' "$BASHRC" 2>/dev/null; then
    echo "[ -f \"$ENV_FILE\" ] && . \"$ENV_FILE\"" >> "$BASHRC"
    ok "Добавлено в ~/.bashrc"
fi

echo ""
if [ "$FAILED" -eq 0 ]; then
    ok "==========================================="
    ok "  Готово. Дальше:"
    ok "      source $ENV_FILE"
    ok "      ./build.sh"
    ok "==========================================="
else
    warn "==========================================="
    warn "  Часть шагов не прошла — см. сообщения выше."
    warn "  Проверить состояние: ./tools/termux-doctor.sh"
    warn "==========================================="
    exit 1
fi
