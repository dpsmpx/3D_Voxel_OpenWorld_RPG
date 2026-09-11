#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# VoxelRPG: сборка APK через gradle (альтернативный путь).
#
# Используется, когда установлен Android SDK с gradle wrapper.
# Требования: gradlew в корне проекта.
# ============================================================

set -e

PROJ="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJ"

MODE="release"
INSTALL=0
for arg in "$@"; do
    case "$arg" in
        debug)   MODE="debug" ;;
        release) MODE="release" ;;
        install) INSTALL=1 ;;
    esac
done

# ---- Проверка gradlew ----
if [ ! -f "./gradlew" ]; then
    echo "==> gradlew не найден — копирую из системного gradle..."

    if ! command -v gradle >/dev/null 2>&1; then
        echo "✗ gradle не найден"
        echo "  Установи: pkg install gradle"
        echo "  Или используй ./build.sh для ручной сборки APK."
        exit 1
    fi

    gradle wrapper --gradle-version 8.4
fi

chmod +x ./gradlew

if [ "$MODE" = "debug" ]; then
    ./gradlew assembleDebug
    APK=$(find app/build/outputs/apk/debug -name "*.apk" | head -1)
else
    ./gradlew assembleRelease
    APK=$(find app/build/outputs/apk/release -name "*.apk" | head -1)
fi

if [ -z "$APK" ]; then
    echo "✗ APK не найден"
    exit 1
fi

echo ""
echo "✓ APK готов: $APK"
echo "  Размер: $(du -h "$APK" | cut -f1)"
echo ""

if [ "$INSTALL" -eq 1 ]; then
    adb install -r "$APK"
fi