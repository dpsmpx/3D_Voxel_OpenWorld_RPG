# --- Компиляция шейдеров ---
SHADERS_SRC="$PROJ/app/src/main/cpp/shaders"
SHADERS_OUT="$PROJ/app/src/main/assets/shaders"
mkdir -p "$SHADERS_OUT"
if command -v glslc >/dev/null 2>&1; then
    for f in "$SHADERS_SRC"/*.vert "$SHADERS_SRC"/*.frag; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        echo "==> glslc: $base"
        glslc -O "$f" -o "$SHADERS_OUT/${base}.spv"
    done
else
    echo "!! glslc не найден — установи пакет: pkg install shaderc"
fi