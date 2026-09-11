#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# VoxelRPG: загрузка на GitHub (вариант A — перезапись).
# ============================================================

set -e

REMOTE="https://github.com/dpsmpx/3D_Voxel_OpenWorld_RPG.git"
PROJ="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJ"

echo "==> Проект: $PROJ"
echo "==> Цель:   $REMOTE"
echo ""

# ---- Проверка git ----
if ! command -v git >/dev/null 2>&1; then
    echo "Устанавливаю git..."
    pkg install -y git
fi

# ---- Проверка настроек ----
if ! git config --global user.email >/dev/null 2>&1; then
    read -p "Ваш email: " EMAIL
    read -p "Ваше имя:  " NAME
    git config --global user.email "$EMAIL"
    git config --global user.name  "$NAME"
fi
git config --global core.filemode false
git config --global credential.helper store

# ---- .gitignore ----
if [ ! -f .gitignore ]; then
    echo "==> Создаю .gitignore..."
    cat > .gitignore << 'EOF'
build/
app/build/
*.apk
*.aab
*.ap_
*.dex
*.class
*.o
*.a
*.d
*.so
.ninja_deps
.ninja_log
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
compile_commands.json
.gradle/
local.properties
captures/
.externalNativeBuild/
.cxx/
app/src/main/jniLibs/
app/src/main/assets/shaders/*.spv
*.keystore
*.jks
*.p12
third_party/
.idea/
*.iml
.vscode/
*.code-workspace
.DS_Store
Thumbs.db
*.swp
*~
*.log
run.sh
deploy.sh
.env
EOF
fi

# ---- Проверка больших файлов ----
echo "==> Проверяю большие файлы..."
BIG=$(find . -type f -not -path './.git/*' -size +50M 2>/dev/null || true)
if [ -n "$BIG" ]; then
    echo "!! Найдены большие файлы (> 50 МБ):"
    echo "$BIG"
    echo "   Добавьте их в .gitignore и повторите."
    exit 1
fi
echo "   OK"

# ---- init ----
if [ ! -d .git ]; then
    git init
    git branch -M main
fi

# ---- remote ----
if git remote | grep -q '^origin$'; then
    git remote set-url origin "$REMOTE"
else
    git remote add origin "$REMOTE"
fi

# ---- add ----
git add .
echo "==> Файлов в индексе: $(git diff --cached --name-only | wc -l)"

# Проверка на бинарники
LEAK=$(git diff --cached --name-only | grep -E '\.(so|apk|keystore|spv)$' || true)
if [ -n "$LEAK" ]; then
    echo "!! В индексе найдены бинарники:"
    echo "$LEAK"
    exit 1
fi

# ---- commit ----
if git diff --cached --quiet; then
    echo "==> Нечего коммитить"
else
    git commit -m "VoxelRPG v1.0.0 — полный исходный код"
fi

# ---- push ----
echo ""
echo "==> Push в origin/main (force)"
echo "    Логин: dpsmpx"
echo "    Пароль: Personal Access Token (не пароль аккаунта!)"
echo "    Где взять: https://github.com/settings/tokens"
echo ""
git push -u origin main --force

echo ""
echo "✓ Готово: https://github.com/dpsmpx/3D_Voxel_OpenWorld_RPG"
