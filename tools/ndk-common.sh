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

# Проверяет, что программа действительно запускается ЗДЕСЬ. Наличия
# файла и бита «исполняемый» мало: пакет мог принести сборку под другую
# архитектуру, и тогда загрузчик Android отвечает «unexpected e_type»,
# а до самой программы дело не доходит.
tool_runs() {                     # путь [аргумент проверки]
    local exe="$1" arg="${2:-}" out rc
    [ -n "$exe" ] && [ -x "$exe" ] || return 1
    out="$("$exe" $arg 2>&1)" && rc=0 || rc=$?
    # Две разные беды, обе не видны по коду выхода:
    #   — файл не для этой архитектуры (загрузчик про e_type);
    #   — файл запускается, но не находит свои библиотеки, потому что
    #     собран под другое окружение. Так выглядит astcenc, собранный
    #     против NDK: ему нужна libc++_shared.so, которой в Termux нет.
    case "$out" in
        *"unexpected e_type"*|*"Exec format error"*|\
        *"cannot execute"*|*"not executable"*|\
        *"CANNOT LINK EXECUTABLE"*|*"error while loading shared libraries"*|\
        *"not found: needed by"*) return 1 ;;
    esac
    # 126 и 127 — оболочка не смогла запустить файл.
    [ "$rc" -ge 126 ] && return 1
    return 0
}

# Первый работающий вариант инструмента SDK. Сначала каталог SDK: в
# сборках под aarch64 там нужные бинарники. Потом PATH — пакет из
# репозитория вполне может оказаться сборкой под другую машину.
find_sdk_tool() {                 # имя [аргумент проверки] -> путь
    local name="$1" arg="${2:-}" root cand
    for root in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" \
                "$HOME/android/android-sdk" "$HOME/android-sdk"; do
        [ -n "$root" ] || continue
        for cand in "$root"/build-tools/*/"$name" "$root/$name"; do
            if tool_runs "$cand" "$arg"; then printf '%s' "$cand"; return 0; fi
        done
    done
    cand="$(command -v "$name" 2>/dev/null || true)"
    if tool_runs "$cand" "$arg"; then printf '%s' "$cand"; return 0; fi
    return 1
}

# Почему инструмент не запускается: путь, цель симлинка, архитектура.
tool_report() {                   # имя
    local name="$1" p real m
    p="$(command -v "$name" 2>/dev/null || true)"
    if [ -z "$p" ]; then
        echo "    $name: в PATH не найден"
        return 0
    fi
    echo "    $name: $p"
    real="$(readlink -f "$p" 2>/dev/null)"
    [ -n "$real" ] || real="$p"
    [ "$real" != "$p" ] && echo "        ссылка на: $real"
    m="$(elf_machine "$real")"
    [ -n "$m" ] && echo "        архитектура: $m"
    return 0
}

# Кодировщик ASTC. Официальные сборки astcenc называются по набору
# инструкций (astcenc-neon, astcenc-sse2, astcenc-avx2, astcenc-native),
# и на aarch64 имени astcenc-native обычно нет вовсе — из-за жёсткой
# проверки одного этого имени сжатие не включалось никогда.
find_astcenc() {                  # -> путь
    local cand root

    # 1. Явно указанный путь.
    if [ -n "${ASTCENC:-}" ] && tool_runs "$ASTCENC" "-help"; then
        printf '%s' "$ASTCENC"; return 0
    fi

    # 2. Собранный tools/build-astcenc.sh — рядом с проектом.
    #    В репозиториях Termux пакета astcenc нет вовсе, поэтому
    #    собственная сборка это основной путь, а не запасной.
    for root in "${PROJ:-.}/tools/astcenc" "$HOME/.voxelrpg/astcenc"; do
        for cand in "$root"/astcenc-*; do
            if tool_runs "$cand" "-help"; then printf '%s' "$cand"; return 0; fi
        done
    done

    # 3. Готовая сборка из распакованных исходников ARM.
    for cand in "$HOME"/astc-encoder*/build*/Source/astcenc-*; do
        if tool_runs "$cand" "-help"; then printf '%s' "$cand"; return 0; fi
    done

    # 4. Что-нибудь из PATH.
    for cand in astcenc astcenc-neon astcenc-native \
                astcenc-sse2 astcenc-sse4.1 astcenc-avx2; do
        cand="$(command -v "$cand" 2>/dev/null || true)"
        if tool_runs "$cand" "-help"; then printf '%s' "$cand"; return 0; fi
    done
    return 1
}

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
