#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# Превращает адреса из отчёта о падении в строки исходников.
#
#   ./tools/symbolize.sh ~/storage/shared/Android/media/com.voxelrpg.game/voxelrpg.log
#
# Без аргумента читает журнал со стандартного ввода.
#
# Работает по несжатой libnative-lib.so из каталога сборки: именно она
# попала в APK, поэтому смещения совпадают. Если библиотеку пересобрали
# после падения — расшифровка будет врать, и скрипт об этом скажет.
# ============================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
. "$PROJ/tools/ndk-common.sh"

SO="${VOXEL_SO:-$PROJ/build/cmake/libnative-lib.so}"
LOG="${1:-}"

C_G='\033[0;32m'; C_Y='\033[1;33m'; C_R='\033[0;31m'; C_0='\033[0m'
err() { printf "${C_R}✗${C_0}  %s\n" "$*" >&2; }

if [ ! -f "$SO" ]; then
    err "Не найдена $SO"
    err "Укажите свою: VOXEL_SO=/путь/к/libnative-lib.so $0 <журнал>"
    exit 1
fi

# addr2line из NDK: обычный из Termux не знает про формат отладочной
# информации, который пишет clang из NDK.
A2L=""
if NDK_TC="$(ndk_toolchain "${ANDROID_NDK_HOME:-$HOME/android/android-ndk}")"; then
    for cand in "$NDK_TC/bin/llvm-addr2line" "$NDK_TC/bin/llvm-symbolizer"; do
        [ -x "$cand" ] && { A2L="$cand"; break; }
    done
fi
[ -n "$A2L" ] || A2L="$(command -v llvm-addr2line || command -v addr2line || true)"
if [ -z "$A2L" ]; then
    err "Не найден addr2line. В Termux: pkg install binutils"
    exit 1
fi

printf "${C_G}библиотека:${C_0} %s\n" "$SO"
printf "${C_G}addr2line:${C_0}  %s\n\n" "$A2L"

# Строки отчёта выглядят так:
#   #00  pc 0000000abcd1  /path/libnative-lib.so  someSymbol
resolve() {
    local line="$1" pc obj
    pc="$(printf '%s' "$line"  | sed -n 's/.*pc \([0-9a-f]\{4,\}\).*/\1/p')"
    obj="$(printf '%s' "$line" | sed -n 's/.*pc [0-9a-f]\{4,\}[[:space:]]*\([^[:space:]]*\).*/\1/p')"
    if [ -z "$pc" ]; then
        printf '%s\n' "$line"
        return
    fi
    # Сверяем по имени файла: в отчёте путь такой, каким он был на
    # телефоне, и с путём к нашей копии он не совпадёт никогда.
    case "$(basename "$obj")" in
        "$(basename "$SO")")
            local out
            out="$("$A2L" -C -f -e "$SO" "0x$pc" 2>/dev/null | tr '\n' ' ')"
            if [ -n "$out" ]; then
                printf '%s\n      %s\n' "$line" "$out"
            else
                printf '%s\n' "$line"
            fi
            ;;
        *) printf '%s  (не наша библиотека)\n' "$line" ;;
    esac
}

if [ -n "$LOG" ]; then
    [ -f "$LOG" ] || { err "Журнал не найден: $LOG"; exit 1; }
    exec < "$LOG"
fi

while IFS= read -r line; do
    case "$line" in
        *"  pc "*) resolve "$line" ;;
        *)         printf '%s\n' "$line" ;;
    esac
done
