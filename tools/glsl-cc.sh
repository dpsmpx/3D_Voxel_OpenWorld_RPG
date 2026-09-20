# ============================================================
# Поиск компилятора GLSL. Подключается из hostcheck и vkcheck:
#
#     . "$PROJ/tools/glsl-cc.sh"
#     glsl_find || { echo "нет компилятора"; exit 1; }
#     glsl_compile in.frag out.spv [-DMODE=1 ...]
#
# Компиляторов два и на разных машинах стои́т разный: glslc приходит
# с shaderc (Termux, Android SDK), glslangValidator — с glslang-tools
# (Debian, Ubuntu, CI). Проверка, которая знает только про один, на
# другой машине молча пропускает шейдеры — а молчащую проверку
# считают пройденной. Ошибку в GLSL при этом не видит ни компилятор
# C++, ни тесты логики: она всплывает на устройстве чёрным экраном.
#
# Здесь нет set -e и нет цвета: файл подключают скрипты с разными
# соглашениями, и он не должен их ломать.
# ============================================================

GLSL_CC=""
GLSL_CC_KIND=""

# Ищет компилятор. Возвращает 0, если нашёлся.
glsl_find() {
    if command -v glslc >/dev/null 2>&1; then
        GLSL_CC="glslc"; GLSL_CC_KIND="glslc"; return 0
    fi
    if command -v glslangValidator >/dev/null 2>&1; then
        GLSL_CC="glslangValidator"; GLSL_CC_KIND="glslang"; return 0
    fi
    return 1
}

glsl_hint() {
    echo "ни glslc, ни glslangValidator не найдены"
    echo "  Termux:       pkg install shaderc"
    echo "  Debian/Ubuntu: apt-get install -y glslang-tools"
}

# Компилирует один шейдер. Лишние аргументы уходят компилятору как
# есть: -D работает одинаково у обоих.
#
# Уровень оптимизации не задаётся намеренно. Проверяется, что GLSL
# КОМПИЛИРУЕТСЯ; настоящие .spv для APK собирает build.sh своим
# компилятором и со своими флагами, и подбирать их здесь значило бы
# заводить вторую правду о сборке.
glsl_compile() {                  # вход, выход, [флаги...]
    local in="$1" out="$2"
    shift 2
    case "$GLSL_CC_KIND" in
        glslc)   glslc "$@" "$in" -o "$out" ;;
        glslang) glslangValidator -V "$@" "$in" -o "$out" ;;
        *)       return 1 ;;
    esac
}
