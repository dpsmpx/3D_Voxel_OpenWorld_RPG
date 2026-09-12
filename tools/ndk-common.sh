# ============================================================
# Общие функции поиска и проверки Android NDK и SDK.
# Подключается из termux-setup.sh, termux-doctor.sh и build.sh:
#
#     . "$PROJ/tools/ndk-common.sh"
#
# Здесь нет вывода в цвете и нет set -e: файл подключают скрипты
# с разными соглашениями, и он не должен их ломать.
# ============================================================

# Архитектура ELF-файла по полю e_machine. Нужна, чтобы отличить
# «NDK не распакован» от «распакован NDK под другую архитектуру»:
# вторая ошибка выглядит как невнятное «unexpected e_type» из
# системного загрузчика Android.
elf_machine() {                   # файл -> aarch64 | x86-64 | ...
    local f="$1" magic code
    [ -f "$f" ] || return 1
    magic="$(od -An -tx1 -N4 "$f" 2>/dev/null | tr -d ' \n')"
    [ "$magic" = "7f454c46" ] || { echo "не ELF"; return 0; }
    code="$(od -An -tx1 -j18 -N2 "$f" 2>/dev/null | tr -d ' \n')"
    case "$code" in
        b700) echo "aarch64" ;;
        3e00) echo "x86-64"  ;;
        2800) echo "arm32"   ;;
        0300) echo "i386"    ;;
        *)    echo "код $code" ;;
    esac
}

# Каталог prebuilt, чей clang РЕАЛЬНО запускается на этой машине.
# Проверять наличие файла мало: в NDK под x86_64 файл тоже есть,
# но на телефоне он не стартует.
ndk_toolchain() {                 # каталог NDK -> путь к prebuilt/<host>
    local d="$1" pb
    [ -n "$d" ] && [ -d "$d" ] || return 1
    for pb in "$d"/toolchains/llvm/prebuilt/*; do
        [ -x "$pb/bin/clang" ] || continue
        if "$pb/bin/clang" --version >/dev/null 2>&1; then
            printf '%s' "$pb"
            return 0
        fi
    done
    return 1
}

# Подробный разбор, почему toolchain не заработал. Печатает в stdout
# всё, что нужно, чтобы понять причину, не имея доступа к телефону.
ndk_toolchain_report() {          # каталог NDK
    local d="$1" pb real mach out found=0
    for pb in "$d"/toolchains/llvm/prebuilt/*; do
        [ -d "$pb" ] || continue
        found=1
        echo "    $(basename "$pb"):"
        if [ ! -e "$pb/bin/clang" ]; then
            echo "        нет bin/clang"
            continue
        fi
        real="$(readlink -f "$pb/bin/clang" 2>/dev/null)"
        [ -n "$real" ] || real="$pb/bin/clang"
        [ "$real" != "$pb/bin/clang" ] && echo "        ссылка на: $real"
        mach="$(elf_machine "$real")"
        [ -n "$mach" ] && echo "        архитектура: $mach"
        out="$("$pb/bin/clang" --version 2>&1 | head -2 | tr '\n' ' ')"
        if "$pb/bin/clang" --version >/dev/null 2>&1; then
            echo "        запускается: да"
        else
            echo "        запускается: НЕТ — $out"
        fi
    done
    [ "$found" -eq 1 ] || echo "    каталог toolchains/llvm/prebuilt пуст или отсутствует"
}

# android.jar нужен только для ручной упаковки APK (aapt2 link -I).
# Версию не фиксируем: подойдёт любая не ниже compileSdk.
find_android_jar() {              # [доп. корень SDK] -> путь к android.jar
    local extra="${1:-}" root jar best=""
    for root in "$extra" "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" \
                "$HOME/android/android-sdk" "$HOME/android-sdk" \
                "${PREFIX:-/usr}/share/android-sdk"; do
        [ -n "$root" ] && [ -d "$root/platforms" ] || continue
        for jar in "$root"/platforms/android-*/android.jar; do
            [ -f "$jar" ] && best="$jar"
        done
        [ -n "$best" ] && { printf '%s' "$best"; return 0; }
    done
    return 1
}

# Компилятор для утилит, которые должны работать НА ЭТОЙ машине
# (атлас текстур). Кросс-компилятор из NDK сюда не годится: он
# соберёт файл под Android, который тут же не запустится. Раньше
# он попадал сюда через PATH и ронял сборку атласа.
host_cxx() {                      # -> путь к рабочему C++ компилятору
    local cand real tmp out
    tmp="$(mktemp -d 2>/dev/null)" || return 1
    printf 'int main(){return 0;}\n' > "$tmp/t.cpp"
    for cand in "${CXX:-}" "${PREFIX:-/usr}/bin/clang++" /usr/bin/clang++ \
                clang++ g++ c++; do
        [ -n "$cand" ] || continue
        command -v "$cand" >/dev/null 2>&1 || continue
        real="$(command -v "$cand")"
        # Компилятор из NDK отбрасываем сразу, даже если он в PATH.
        case "$(readlink -f "$real" 2>/dev/null || printf '%s' "$real")" in
            *"/toolchains/llvm/prebuilt/"*) continue ;;
        esac
        # Решает не наличие файла, а то, что он собирает работающий код.
        if out="$("$real" -o "$tmp/t" "$tmp/t.cpp" 2>&1)" && "$tmp/t"; then
            rm -rf "$tmp"
            printf '%s' "$real"
            return 0
        fi
    done
    rm -rf "$tmp"
    return 1
}

# Каталоги prebuilt, чей clang на этой машине не запускается.
# На телефоне это почти всегда linux-x86_64 из официального NDK:
# лишний гигабайт, который вдобавок сбивает с толку android.toolchain.cmake.
ndk_foreign_toolchains() {        # каталог NDK -> список путей
    local d="$1" pb
    [ -d "$d" ] || return 1
    for pb in "$d"/toolchains/llvm/prebuilt/*; do
        [ -e "$pb/bin/clang" ] || continue
        "$pb/bin/clang" --version >/dev/null 2>&1 || printf '%s\n' "$pb"
    done
}

# Найдёт ли CMake через android.toolchain.cmake рабочий компилятор.
#
# В официальном NDK хост-тег зашит как linux-x86_64, про aarch64 файл
# не знает, и на телефоне CMake зовёт компилятор, которого там быть
# не может: «"...linux-x86_64/bin/clang-NN" has unexpected e_type: 2».
#
# Важен результат, а не текст файла: если prebuilt/linux-x86_64 —
# симлинк на рабочий toolchain, зашитый тег никому не мешает.
# Код: 0 — компилятор найдётся, 1 — нет, 2 — файла нет.
ndk_cmake_host_ok() {             # каталог NDK
    local d="$1" f="$1/build/cmake/android.toolchain.cmake"
    [ -f "$f" ] || return 2
    grep -q 'linux-aarch64' "$f" && return 0
    local fallback="$d/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
    [ -x "$fallback" ] && "$fallback" --version >/dev/null 2>&1 && return 0
    return 1
}
