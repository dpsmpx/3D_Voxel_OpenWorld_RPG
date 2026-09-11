#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# Проверка окружения сборки. Ничего не устанавливает — только
# показывает, что есть, чего нет и что с этим делать.
#
#   ./tools/termux-doctor.sh
# ============================================================

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
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
want "aapt2"          aapt2     "pkg install aapt2 (может потребоваться pkg install tur-repo)"
want "apksigner"      apksigner "pkg install apksigner"
want "zipalign"       zipalign  "входит в build-tools"
want "astcenc"        astcenc-native "сжатие атласа в ASTC, необязательно"
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

    # Имя каталога prebuilt у разных сборок разное — ищем по шаблону,
    # иначе рабочий NDK объявляется сломанным из-за имени каталога.
    TC="$NDK/toolchains/llvm/prebuilt/linux-aarch64"
    for _pb in "$NDK"/toolchains/llvm/prebuilt/*; do
        [ -x "$_pb/bin/clang" ] && { TC="$_pb"; break; }
    done
    if [ -x "$TC/bin/clang" ]; then
        printf "${C_G}  ✓${C_0} %-22s %s\n" "toolchain aarch64" "$TC"
    else
        printf "${C_R}  ✗${C_0} %-22s нет %s\n" "toolchain aarch64" "$TC/bin/clang"
        printf "      Скорее всего скачан NDK под x86_64 — на телефоне он не запустится.\n"
        printf "      Нужна сборка под linux-aarch64: github.com/lzhiyong/termux-ndk\n"
        MISSING=$((MISSING + 1))
    fi

    GLUE="$NDK/sources/android/native_app_glue/android_native_app_glue.c"
    if [ -f "$GLUE" ]; then
        printf "${C_G}  ✓${C_0} %-22s\n" "native_app_glue"
    else
        printf "${C_R}  ✗${C_0} %-22s нет %s\n" "native_app_glue" "$GLUE"
        MISSING=$((MISSING + 1))
    fi

    if [ -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
        printf "${C_G}  ✓${C_0} %-22s\n" "android.toolchain.cmake"
    else
        printf "${C_R}  ✗${C_0} %-22s нет\n" "android.toolchain.cmake"
        MISSING=$((MISSING + 1))
    fi
fi

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
