#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# Проверка окружения сборки. Ничего не устанавливает — только
# показывает, что есть, чего нет и что с этим делать.
#
#   ./tools/termux-doctor.sh
# ============================================================

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
. "$PROJ/tools/ndk-common.sh"

C_G='\033[0;32m'; C_Y='\033[1;33m'; C_R='\033[0;31m'; C_0='\033[0m'

MISSING=0
WARNED=0

need() {   # need <что> <команда> <как починить>
    if command -v "$2" >/dev/null 2>&1; then
        printf "${C_G}  ✓${C_0} %-22s %s\n" "$1" "$(command -v "$2")"
    else
        printf "${C_R}  ✗${C_0} %-22s нет — %s\n" "$1" "$3"
        MISSING=$((MISSING + 1))
    fi
}

want() {   # необязательное
    if command -v "$2" >/dev/null 2>&1; then
        printf "${C_G}  ✓${C_0} %-22s %s\n" "$1" "$(command -v "$2")"
    else
        printf "${C_Y}  !${C_0} %-22s нет — %s\n" "$1" "$3"
        WARNED=$((WARNED + 1))
    fi
}

echo ""
echo "Окружение сборки VoxelRPG"
echo "========================="
echo ""
echo "Инструменты:"
need "компилятор"     clang     "pkg install clang"
need "cmake"          cmake     "pkg install cmake"
need "ninja"          ninja     "pkg install ninja"
need "git"            git       "pkg install git"
need "zip"            zip       "pkg install zip"
need "java"           java      "pkg install openjdk-21"
want "glslc"          glslc     "pkg install shaderc"
# Инструменты упаковки проверяем запуском: пакет aapt2 в Termux бывает
# собран под x86_64, и тогда наличие файла ничего не значит — сборка
# APK падает с «unexpected e_type», а по имени пакета это не понять.
check_sdk_tool() {   # <что> <команда> [аргумент проверки]
    local label="$1" name="$2" arg="${3:-}" path
    if path="$(find_sdk_tool "$name" "$arg")"; then
        printf "${C_G}  ✓${C_0} %-22s %s\n" "$label" "$path"
        return 0
    fi
    printf "${C_Y}  !${C_0} %-22s рабочего нет — APK не упакуется\n" "$label"
    tool_report "$name"
    printf "      Сборка build-tools под aarch64:\n"
    printf "          ./tools/termux-setup.sh --skip-packages --sdk <архив android-sdk>\n"
    WARNED=$((WARNED + 1))
    return 1
}

check_sdk_tool "aapt2"     aapt2     version
check_sdk_tool "apksigner" apksigner --version
check_sdk_tool "zipalign"  zipalign

want "adb"            adb       "pkg install android-tools, для установки на устройство"

echo ""
echo "Android NDK:"
# Подсказка об установке — одна на все ветки, чтобы не расходилась.
ndk_hint() {
    printf "      В pkg пакета termux-ndk НЕТ. Автоматически:\n"
    printf "          ./tools/termux-setup.sh\n"
    printf "      Если GitHub недоступен — скачайте android-ndk-*-aarch64.zip со\n"
    printf "      страницы https://github.com/lzhiyong/termux-ndk/releases и укажите архив:\n"
    printf "          ./tools/termux-setup.sh --ndk ~/storage/downloads/android-ndk-....zip\n"
}

NDK="${ANDROID_NDK_HOME:-}"
# Переменной нет, а NDK лежит на штатном месте — разница важная:
# надо всего лишь сделать source, а не переустанавливать гигабайт.
if [ -z "$NDK" ] && [ -d "${HOME:-/}/android/android-ndk" ]; then
    printf "${C_Y}  !${C_0} ANDROID_NDK_HOME не задана, но NDK найден: %s\n" "$HOME/android/android-ndk"
    printf "      Выполните: source ~/.voxelrpg-env\n"
    NDK="$HOME/android/android-ndk"
fi
if [ -z "$NDK" ]; then
    printf "${C_R}  ✗${C_0} ANDROID_NDK_HOME не задана\n"
    ndk_hint
    MISSING=$((MISSING + 1))
elif [ ! -d "$NDK" ]; then
    printf "${C_R}  ✗${C_0} ANDROID_NDK_HOME указывает в никуда: %s\n" "$NDK"
    ndk_hint
    MISSING=$((MISSING + 1))
else
    printf "${C_G}  ✓${C_0} %-22s %s\n" "ANDROID_NDK_HOME" "$NDK"

    # Наличия файла мало: NDK под x86_64 распакован ровно так же и
    # отличается только тем, что его clang здесь не запускается.
    if TC="$(ndk_toolchain "$NDK")"; then
        printf "${C_G}  ✓${C_0} %-22s %s\n" "clang (запускается)" "$TC/bin/clang"
    else
        printf "${C_R}  ✗${C_0} %-22s ни один не запускается\n" "clang"
        ndk_toolchain_report "$NDK"
        printf "      Если архитектура не aarch64 — это NDK под x86_64; на телефоне\n"
        printf "      он не стартует. Нужна сборка под linux-aarch64:\n"
        printf "      https://github.com/lzhiyong/termux-ndk/releases\n"
        MISSING=$((MISSING + 1))
    fi

    GLUE="$NDK/sources/android/native_app_glue/android_native_app_glue.c"
    if [ -f "$GLUE" ]; then
        printf "${C_G}  ✓${C_0} %-22s\n" "native_app_glue"
    else
        printf "${C_R}  ✗${C_0} %-22s нет %s\n" "native_app_glue" "$GLUE"
        MISSING=$((MISSING + 1))
    fi

    # Мало, чтобы файл был: в официальном NDK хост-тег зашит как
    # linux-x86_64, и на телефоне CMake ищет компилятор не там.
    ndk_cmake_host_ok "$NDK"
    case "$?" in
        0) if grep -q 'linux-aarch64' "$NDK/build/cmake/android.toolchain.cmake"; then
               printf "${C_G}  ✓${C_0} %-22s знает хост aarch64\n" "android.toolchain.cmake"
           else
               printf "${C_G}  ✓${C_0} %-22s хост-тег зашит, но ведёт\n" "android.toolchain.cmake"
               printf "      на рабочий toolchain (симлинк prebuilt/linux-x86_64)\n"
           fi ;;
        1) printf "${C_R}  ✗${C_0} %-22s не знает хост aarch64\n" "android.toolchain.cmake"
           printf "      CMake уйдёт в prebuilt/linux-x86_64 и упадёт с unexpected e_type.\n"
           printf "      Починить: ./tools/termux-setup.sh --fix-ndk\n"
           MISSING=$((MISSING + 1)) ;;
        *) printf "${C_R}  ✗${C_0} %-22s нет файла\n" "android.toolchain.cmake"
           MISSING=$((MISSING + 1)) ;;
    esac

    FOREIGN="$(ndk_foreign_toolchains "$NDK" || true)"
    if [ -n "$FOREIGN" ]; then
        printf "${C_Y}  !${C_0} есть toolchain не для этой машины:\n"
        printf '%s\n' "$FOREIGN" | sed 's|^|        |'
        printf "      Убрать (освободит ~1 ГБ): ./tools/termux-setup.sh --fix-ndk\n"
        WARNED=$((WARNED + 1))
    fi
fi

echo ""
echo "Android SDK (нужен только android.jar для упаковки APK):"
if JAR="$(find_android_jar)"; then
    printf "${C_G}  ✓${C_0} %-22s %s\n" "android.jar" "$JAR"
else
    printf "${C_Y}  !${C_0} %-22s нет — APK не упакуется, .so соберётся\n" "android.jar"
    printf "      ./tools/termux-setup.sh --skip-packages --sdk <архив SDK>\n"
    WARNED=$((WARNED + 1))
fi

# Кросс-компилятор в PATH ломает сборку хостовых утилит.
# Ошибка при этом выглядит как «unexpected e_type», и связь с PATH
# по ней не видна — поэтому проверяем отдельно.
echo ""
echo "Хостовый компилятор:"
if HCXX="$(host_cxx)"; then
    printf "${C_G}  ✓${C_0} %-22s %s\n" "c++ для хоста" "$HCXX"
else
    printf "${C_R}  ✗${C_0} %-22s нет — pkg install clang\n" "c++ для хоста"
    MISSING=$((MISSING + 1))
fi
case "$(command -v clang++ 2>/dev/null)" in
    *"/toolchains/llvm/prebuilt/"*)
        printf "${C_Y}  !${C_0} clang++ из PATH — кросс-компилятор NDK: %s\n" "$(command -v clang++)"
        printf "      Уберите каталог NDK из PATH: он подменяет компилятор Termux.\n"
        WARNED=$((WARNED + 1)) ;;
esac

echo ""
echo "Зависимости проекта:"
for d in "glm/glm:GLM" "entt/include/entt/entt.hpp:EnTT"; do
    path="${d%%:*}"; name="${d##*:}"
    if [ -e "$PROJ/third_party/$path" ]; then
        printf "${C_G}  ✓${C_0} %-22s third_party/%s\n" "$name" "${path%%/*}"
    else
        printf "${C_Y}  !${C_0} %-22s нет — скачает build.sh\n" "$name"
        WARNED=$((WARNED + 1))
    fi
done

echo ""
echo "Платформа:"
printf "     архитектура: %s\n" "$(uname -m)"
printf "     ядро:        %s\n" "$(uname -r)"
if [ "$(uname -m)" != "aarch64" ]; then
    printf "${C_Y}  !${C_0} Проект рассчитан на aarch64\n"
    WARNED=$((WARNED + 1))
fi

echo ""
if [ "$MISSING" -eq 0 ]; then
    printf "${C_G}Всё необходимое на месте.${C_0} Запускайте: ./build.sh\n"
    [ "$WARNED" -gt 0 ] && printf "Необязательного не хватает: %d — часть шагов будет пропущена.\n" "$WARNED"
    echo ""
    exit 0
else
    printf "${C_R}Не хватает обязательного: %d.${C_0}\n" "$MISSING"
    printf "Проще всего исправить одной командой: ${C_G}./tools/termux-setup.sh${C_0}\n"
    echo ""
    exit 1
fi
