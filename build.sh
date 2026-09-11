#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# VoxelRPG: скрипт сборки APK в Termux.
#
# Требования:
#   pkg install termux-ndk cmake ninja git clang shaderc
#   pkg install openjdk-17 aapt2 apksigner d8
#
# Использование:
#   ./build.sh              — release APK
#   ./build.sh debug        — debug APK
#   ./build.sh clean        — очистка
#   ./build.sh install      — собрать и установить через adb
# ============================================================

set -e

# ---- Настройки ----
PROJ="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$PROJ/app"
SRC_DIR="$APP_DIR/src/main"
CPP_DIR="$SRC_DIR/cpp"
ASSETS_DIR="$SRC_DIR/assets/shaders"
JNI_DIR="$SRC_DIR/jniLibs/arm64-v8a"

BUILD_DIR="$PROJ/build"
CMAKE_BUILD_DIR="$BUILD_DIR/cmake"
APK_OUT_DIR="$BUILD_DIR/apk"

ABI="arm64-v8a"
API=24

NDK_HOME="${ANDROID_NDK_HOME:-$PREFIX/lib/android-ndk}"
TOOLCHAIN="$NDK_HOME/toolchains/llvm/prebuilt/linux-aarch64"
SYSROOT="$TOOLCHAIN/sysroot"

# Android SDK для aapt2 / apksigner / d8
ANDROID_HOME="${ANDROID_HOME:-$HOME/android-sdk}"
BUILD_TOOLS_VERSION="${BUILD_TOOLS_VERSION:-34.0.0}"
BUILD_TOOLS="$ANDROID_HOME/build-tools/$BUILD_TOOLS_VERSION"
ANDROID_JAR="$ANDROID_HOME/platforms/android-34/android.jar"

# Ключ для подписи debug
DEBUG_KEYSTORE="$APP_DIR/debug.keystore"

# Имя итогового APK
GAME_NAME="VoxelRPG"

# ---- Цветной вывод ----
C_RED='\033[0;31m'
C_GREEN='\033[0;32m'
C_YELLOW='\033[1;33m'
C_BLUE='\033[0;34m'
C_RESET='\033[0m'

log()  { printf "${C_BLUE}==>${C_RESET} %s\n" "$*"; }
ok()   { printf "${C_GREEN}✓${C_RESET}  %s\n" "$*"; }
warn() { printf "${C_YELLOW}!${C_RESET}  %s\n" "$*"; }
err()  { printf "${C_RED}✗${C_RESET}  %s\n" "$*" >&2; }

# ---- Параметры ----
MODE="release"
INSTALL=0
for arg in "$@"; do
    case "$arg" in
        debug)   MODE="debug" ;;
        release) MODE="release" ;;
        clean)
            log "Очистка..."
            rm -rf "$BUILD_DIR"
            rm -rf "$JNI_DIR"
            rm -rf "$ASSETS_DIR"
            ok "Очищено"
            exit 0
            ;;
        install) INSTALL=1 ;;
        *) warn "Неизвестный аргумент: $arg" ;;
    esac
done

# ---- Проверка окружения ----
log "Проверка окружения..."

if [ ! -d "$NDK_HOME" ]; then
    err "NDK не найден: $NDK_HOME"
    err "Установи: pkg install termux-ndk"
    exit 1
fi

if [ ! -d "$TOOLCHAIN" ]; then
    err "Toolchain не найден: $TOOLCHAIN"
    exit 1
fi

if [ ! -f "$ANDROID_JAR" ]; then
    warn "android.jar не найден ($ANDROID_JAR) — ручная сборка APK пропущена"
    warn "Установи SDK или используй gradle."
    USE_GRADLE=1
else
    USE_GRADLE=0
fi

# ---- GLM ----
if [ ! -d "$PROJ/third_party/glm" ]; then
    log "Клонирую GLM..."
    mkdir -p "$PROJ/third_party"
    git clone --depth=1 https://github.com/g-truc/glm.git "$PROJ/third_party/glm"
    ok "GLM готов"
fi

# ---- Компиляция шейдеров ----
log "Компиляция шейдеров..."
mkdir -p "$ASSETS_DIR"

if ! command -v glslc >/dev/null 2>&1; then
    err "glslc не найден — установи: pkg install shaderc"
    exit 1
fi

SHADERS_SRC="$CPP_DIR/shaders"
if [ ! -d "$SHADERS_SRC" ]; then
    err "Директория шейдеров не найдена: $SHADERS_SRC"
    exit 1
fi

SHADER_COUNT=0
for f in "$SHADERS_SRC"/*.vert "$SHADERS_SRC"/*.frag "$SHADERS_SRC"/*.comp; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    echo "  - $base"
    glslc -O "$f" -o "$ASSETS_DIR/${base}.spv"
    SHADER_COUNT=$((SHADER_COUNT + 1))
done

if [ "$SHADER_COUNT" -eq 0 ]; then
    warn "Не найдено ни одного шейдера в $SHADERS_SRC"
else
    ok "Скомпилировано шейдеров: $SHADER_COUNT"
fi

# ---- CMake ----
log "Конфигурация CMake ($MODE)..."

mkdir -p "$CMAKE_BUILD_DIR"
cd "$CMAKE_BUILD_DIR"

CMAKE_BUILD_TYPE="Release"
if [ "$MODE" = "debug" ]; then
    CMAKE_BUILD_TYPE="Debug"
fi

cmake "$CPP_DIR" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL="c++_shared" \
    -DANDROID_ARM_NEON=ON \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

ok "CMake настроен"

# ---- Сборка ----
log "Сборка native библиотеки..."
ninja -j"$(nproc)"

if [ ! -f "libnative-lib.so" ]; then
    err "libnative-lib.so не создан"
    exit 1
fi

mkdir -p "$JNI_DIR"
cp libnative-lib.so "$JNI_DIR/"

# Копируем libc++_shared.so — обязательна для c++_shared.
if [ -f "$SYSROOT/usr/lib/aarch64-linux-android/libc++_shared.so" ]; then
    cp "$SYSROOT/usr/lib/aarch64-linux-android/libc++_shared.so" "$JNI_DIR/"
    ok "libc++_shared.so скопирована"
else
    warn "libc++_shared.so не найдена в sysroot"
fi

SO_SIZE=$(du -h "$JNI_DIR/libnative-lib.so" | cut -f1)
ok "Native собран: libnative-lib.so ($SO_SIZE)"

# ---- Упаковка APK ----
if [ "$USE_GRADLE" -eq 1 ]; then
    log "android.jar не найден — попытка использовать gradle..."
    if command -v gradle >/dev/null 2>&1; then
        cd "$PROJ"
        ./gradlew assembleRelease
        ok "Собрано через gradle"
        exit 0
    else
        err "gradle не найден. Установи Android SDK или gradle."
        exit 1
    fi
fi

log "Упаковка APK (ручная)..."

mkdir -p "$APK_OUT_DIR"
cd "$APK_OUT_DIR"
rm -f *.apk *.zip *.arsc *.dex resources.ap_ 2>/dev/null || true

# ---- aapt2 compile ----
log "aapt2 compile..."

RES_DIR="$SRC_DIR/res"
RES_ZIP="$APK_OUT_DIR/res.zip"

if [ ! -d "$RES_DIR" ]; then
    err "Директория ресурсов не найдена: $RES_DIR"
    err "Манифест ссылается на @mipmap/ic_launcher — без res сборка не пройдёт."
    exit 1
fi
aapt2 compile --dir "$RES_DIR" -o "$RES_ZIP"

# ---- aapt2 link ----
log "aapt2 link..."

MANIFEST="$SRC_DIR/AndroidManifest.xml"
if [ ! -f "$MANIFEST" ]; then
    err "AndroidManifest.xml не найден"
    exit 1
fi

aapt2 link \
    -o base.apk \
    -I "$ANDROID_JAR" \
    --manifest "$MANIFEST" \
    --min-sdk-version "$API" \
    --target-sdk-version 34 \
    --version-code 1 \
    --version-name "1.0.0" \
    --no-version-vectors \
    -A "$SRC_DIR/assets" \
    --auto-add-overlay \
    "$RES_ZIP"

if [ ! -f "base.apk" ]; then
    err "aapt2 link не создал base.apk"
    exit 1
fi
ok "base.apk создан"

# ---- Добавляем native libs и assets ----
log "Добавляем native libs и assets..."

# aapt2 уже включил assets, но .so нужно добавить вручную.
cd "$APK_OUT_DIR"

# Копируем .so в staging
STAGING="$APK_OUT_DIR/staging"
rm -rf "$STAGING"
mkdir -p "$STAGING/lib/$ABI"
cp "$JNI_DIR"/*.so "$STAGING/lib/$ABI/"

# Манифест объявляет extractNativeLibs="false": система грузит .so
# прямо из APK, поэтому они должны лежать без сжатия (-0), иначе
# установка пройдёт, а запуск — нет.
cd "$STAGING"
zip -q -0 -r ../base.apk lib

cd "$APK_OUT_DIR"
ok "Native libs добавлены (без сжатия)"

# ---- zipalign ----
log "zipalign..."
if command -v zipalign >/dev/null 2>&1; then
    # -p выравнивает .so по границе страницы: обязательно для
    # extractNativeLibs="false".
    zipalign -f -p 4 base.apk aligned.apk
    mv aligned.apk base.apk
    ok "zipalign выполнен"
else
    err "zipalign не найден, а он обязателен при extractNativeLibs=false"
    err "Установи Android SDK build-tools"
    exit 1
fi

# ---- Подпись ----
log "Подпись APK..."

# Release-ключ, если он есть; иначе debug.
RELEASE_KEYSTORE="$APP_DIR/release.keystore"
if [ "$MODE" = "release" ] && [ -f "$RELEASE_KEYSTORE" ] \
   && command -v apksigner >/dev/null 2>&1; then
    log "Подписываю release-ключом..."
    apksigner sign \
        --ks "$RELEASE_KEYSTORE" \
        --ks-pass "pass:${RELEASE_KEYSTORE_PASSWORD:-release}" \
        --ks-key-alias "${RELEASE_KEY_ALIAS:-release}" \
        --key-pass "pass:${RELEASE_KEY_PASSWORD:-release}" \
        --out "$APK_OUT_DIR/${GAME_NAME}.apk" \
        base.apk
    ok "APK подписан release-ключом"
    SKIP_DEBUG_SIGN=1
else
    SKIP_DEBUG_SIGN=0
fi

# Создаём debug keystore, если его нет.
if [ "$SKIP_DEBUG_SIGN" -eq 0 ] && [ ! -f "$DEBUG_KEYSTORE" ]; then
    log "Создаю debug keystore..."
    keytool -genkeypair \
        -keystore "$DEBUG_KEYSTORE" \
        -storepass android \
        -alias androiddebugkey \
        -keypass android \
        -keyalg RSA \
        -keysize 2048 \
        -validity 10000 \
        -dname "CN=Android Debug, O=Android, C=US" 2>/dev/null || {
            warn "keytool не найден — APK не подписан"
            warn "Установи: pkg install openjdk-17"
            mv base.apk "$APK_OUT_DIR/${GAME_NAME}.apk"
            exit 0
        }
fi

if [ "$SKIP_DEBUG_SIGN" -eq 1 ]; then
    :   # уже подписан release-ключом
elif command -v apksigner >/dev/null 2>&1; then
    apksigner sign \
        --ks "$DEBUG_KEYSTORE" \
        --ks-pass pass:android \
        --ks-key-alias androiddebugkey \
        --key-pass pass:android \
        --out "$APK_OUT_DIR/${GAME_NAME}.apk" \
        base.apk
    ok "APK подписан"
else
    warn "apksigner не найден — копирую без подписи"
    mv base.apk "$APK_OUT_DIR/${GAME_NAME}.apk"
fi

# ---- Итог ----
FINAL_APK="$APK_OUT_DIR/${GAME_NAME}.apk"
if [ -f "$FINAL_APK" ]; then
    FINAL_SIZE=$(du -h "$FINAL_APK" | cut -f1)
    echo ""
    ok "==========================================="
    ok "  APK готов: $FINAL_APK"
    ok "  Размер: $FINAL_SIZE"
    ok "  Mode: $MODE"
    ok "==========================================="
    echo ""
else
    err "APK не создан"
    exit 1
fi

# ---- Установка через adb ----
if [ "$INSTALL" -eq 1 ]; then
    log "Установка через adb..."
    if command -v adb >/dev/null 2>&1; then
        adb install -r "$FINAL_APK"
        ok "Установлено"
    else
        warn "adb не найден. Установи вручную: $FINAL_APK"
    fi
fi