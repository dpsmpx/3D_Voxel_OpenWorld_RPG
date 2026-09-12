#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# VoxelRPG: скрипт сборки APK в Termux.
#
# Подготовка окружения — один раз:
#   ./tools/termux-setup.sh
#   source ~/.voxelrpg-env
#
# Проверить, чего не хватает:
#   ./tools/termux-doctor.sh
#
# Внимание: пакета termux-ndk в Termux НЕ существует, а имя пакета
# Java менялось (openjdk-17 -> openjdk-21). Поэтому установка вынесена
# в termux-setup.sh, который определяет доступные имена сам.
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

. "$PROJ/tools/ndk-common.sh"

NDK_HOME="${ANDROID_NDK_HOME:-$PREFIX/lib/android-ndk}"
TOOLCHAIN="$NDK_HOME/toolchains/llvm/prebuilt/linux-aarch64"
SYSROOT="$TOOLCHAIN/sysroot"

# Android SDK нужен ровно ради android.jar (aapt2 link -I).
# aapt2/apksigner/zipalign берутся из PATH — в Termux они из pkg.
# Путь и версия не зашиты: SDK ставится в разные места, а подойдёт
# любая платформа не ниже compileSdk.
ANDROID_HOME="${ANDROID_HOME:-$HOME/android/android-sdk}"
ANDROID_JAR=""

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

# Подхватываем переменные, записанные termux-setup.sh, если текущая
# оболочка их ещё не видит.
if [ -z "${ANDROID_NDK_HOME:-}" ] && [ -f "$HOME/.voxelrpg-env" ]; then
    . "$HOME/.voxelrpg-env"
    NDK_HOME="${ANDROID_NDK_HOME:-$NDK_HOME}"
fi

# Штатное место установки termux-setup.sh — на случай, если ~/.voxelrpg-env
# потёрт, но сам NDK на диске остался.
if [ ! -d "$NDK_HOME" ] && [ -d "$HOME/android/android-ndk" ]; then
    NDK_HOME="$HOME/android/android-ndk"
fi

# Берём тот каталог prebuilt, чей clang реально запускается на этой
# машине: имя каталога ничего не гарантирует, а NDK под x86_64
# распакован ровно так же и отличается только тем, что не работает.
if _tc="$(ndk_toolchain "$NDK_HOME")"; then
    TOOLCHAIN="$_tc"
fi
SYSROOT="$TOOLCHAIN/sysroot"

if [ ! -d "$NDK_HOME" ]; then
    err "Android NDK не найден: $NDK_HOME"
    err ""
    err "Пакета termux-ndk в Termux нет — официальный NDK собран под"
    err "x86_64 и на телефоне не запустится. Нужна сборка под aarch64."
    err ""
    err "Установить всё разом:"
    err "    ./tools/termux-setup.sh && source ~/.voxelrpg-env"
    err ""
    err "Если GitHub недоступен, скачайте android-ndk-*-aarch64.zip вручную"
    err "с https://github.com/lzhiyong/termux-ndk/releases и укажите архив:"
    err "    ./tools/termux-setup.sh --ndk ~/storage/downloads/android-ndk-....zip"
    err ""
    err "Посмотреть, чего именно не хватает:"
    err "    ./tools/termux-doctor.sh"
    exit 1
fi

if ! ndk_toolchain "$NDK_HOME" >/dev/null; then
    err "В NDK нет работоспособного clang. Что нашлось в $NDK_HOME:"
    ndk_toolchain_report "$NDK_HOME" >&2
    err ""
    err "Если архитектура выше не aarch64 — это NDK под x86_64."
    err "На телефоне он не запускается: системный загрузчик Android"
    err "отвергает такой файл с сообщением про unexpected e_type."
    err "Нужна сборка под linux-aarch64:"
    err "    https://github.com/lzhiyong/termux-ndk/releases"
    err "    ./tools/termux-setup.sh --ndk <скачанный архив>"
    exit 1
fi

# Официальный android.toolchain.cmake знает только хост linux-x86_64.
# На телефоне из-за этого CMake зовёт компилятор не из того каталога
# и падает с «unexpected e_type: 2» — ошибкой, по которой причина не
# читается совсем. Чиним не здесь (это не дело сборки), но говорим,
# что именно запустить.
if ! ndk_cmake_host_ok "$NDK_HOME"; then
    err "android.toolchain.cmake в этом NDK не знает про хост aarch64:"
    err "    $NDK_HOME/build/cmake/android.toolchain.cmake"
    err "CMake будет искать компилятор в prebuilt/linux-x86_64 и упадёт"
    err "с сообщением про unexpected e_type. Почините одной командой:"
    err "    ./tools/termux-setup.sh --fix-ndk"
    exit 1
fi

_foreign="$(ndk_foreign_toolchains "$NDK_HOME" || true)"
if [ -n "$_foreign" ]; then
    warn "В NDK есть toolchain, который здесь не запускается:"
    printf '%s\n' "$_foreign" | sed 's/^/      /'
    warn "Он занимает место и путает CMake. Убрать: ./tools/termux-setup.sh --fix-ndk"
fi

# glue из NDK — без него не соберётся точка входа.
if [ ! -f "$NDK_HOME/sources/android/native_app_glue/android_native_app_glue.c" ]; then
    err "В NDK нет native_app_glue:"
    err "    $NDK_HOME/sources/android/native_app_glue/"
    err "NDK распакован не полностью. Проверьте: ./tools/termux-doctor.sh"
    exit 1
fi

ANDROID_JAR="$(find_android_jar "$ANDROID_HOME" || true)"
if [ -z "$ANDROID_JAR" ]; then
    warn "android.jar не найден — ручная сборка APK пропущена"
    warn "Искали в: \$ANDROID_HOME, ~/android/android-sdk, ~/android-sdk,"
    warn "          \$PREFIX/share/android-sdk (platforms/android-*/android.jar)"
    warn "Поставить: ./tools/termux-setup.sh --skip-packages --sdk <архив SDK>"
    warn "Установи SDK или используй gradle."
    USE_GRADLE=1
else
    USE_GRADLE=0
fi

# ---- Заголовочные зависимости ----
mkdir -p "$PROJ/third_party"

if [ ! -d "$PROJ/third_party/glm/glm" ]; then
    log "Клонирую GLM..."
    git -c advice.detachedHead=false clone --depth=1 https://github.com/g-truc/glm.git "$PROJ/third_party/glm"
    ok "GLM готов"
fi

# EnTT — ECS из ТЗ 3.2. Нужен только single-header.
if [ ! -f "$PROJ/third_party/entt/include/entt/entt.hpp" ]; then
    log "Клонирую EnTT..."
    rm -rf "$PROJ/third_party/entt-src"
    git -c advice.detachedHead=false clone --depth=1 --branch v3.13.2 \
        https://github.com/skypjack/entt.git "$PROJ/third_party/entt-src"
    mkdir -p "$PROJ/third_party/entt/include/entt"
    cp "$PROJ/third_party/entt-src/single_include/entt/entt.hpp" \
       "$PROJ/third_party/entt/include/entt/"
    rm -rf "$PROJ/third_party/entt-src"
    ok "EnTT готов"
fi

# ---- Компиляция шейдеров ----
log "Компиляция шейдеров..."
mkdir -p "$ASSETS_DIR"

if ! command -v glslc >/dev/null 2>&1; then
    err "glslc не найден. Обычно он в пакете shaderc:"
    err "    pkg install shaderc"
    err "Если пакета нет — посмотрите, как он называется: pkg search glsl"
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

# ---- Атлас блоков в ASTC (ТЗ 3.3) ----
# Шаг необязательный: без astcenc игра соберёт атлас процедурно
# при старте, только он займёт вчетверо больше видеопамяти.
log "Атлас блоков..."
"$PROJ/tools/atlas/build.sh" "$SRC_DIR/assets" || \
    warn "Атлас в ASTC не собран — будет процедурный RGBA8"

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
            warn "Установи JDK: pkg install openjdk-21"
            warn "(если такого пакета нет: pkg search openjdk)"
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