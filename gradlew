#!/bin/sh
# ============================================================
# Gradle wrapper. Скачивает дистрибутив, указанный в
# gradle/wrapper/gradle-wrapper.properties, и запускает сборку.
#
# gradle-wrapper.jar в репозиторий не коммитится (бинарник), вместо
# него дистрибутив скачивается напрямую — так же, как это делает
# сам wrapper, только без промежуточного jar.
# ============================================================
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
PROPS="$DIR/gradle/wrapper/gradle-wrapper.properties"

if [ ! -f "$PROPS" ]; then
    echo "gradlew: не найден $PROPS" >&2
    exit 1
fi

# Если gradle-wrapper.jar на месте — используем штатный путь.
if [ -f "$DIR/gradle/wrapper/gradle-wrapper.jar" ]; then
    exec java -classpath "$DIR/gradle/wrapper/gradle-wrapper.jar" \
        org.gradle.wrapper.GradleWrapperMain "$@"
fi

DIST_URL=$(sed -n 's/^distributionUrl=//p' "$PROPS" | sed 's|\\:|:|g')
VERSION=$(echo "$DIST_URL" | sed -n 's|.*/gradle-\([0-9.]*\)-.*|\1|p')
HOME_DIR="${GRADLE_USER_HOME:-$HOME/.gradle}"
TARGET="$HOME_DIR/wrapper/dists/gradle-$VERSION"

if [ ! -x "$TARGET/bin/gradle" ]; then
    echo "==> Скачиваю Gradle $VERSION..."
    mkdir -p "$TARGET"
    TMP="$TARGET/gradle.zip"
    if command -v curl >/dev/null 2>&1; then
        curl -fSL -o "$TMP" "$DIST_URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$TMP" "$DIST_URL"
    else
        echo "gradlew: нужен curl или wget" >&2
        exit 1
    fi
    unzip -q -o "$TMP" -d "$TARGET.tmp"
    mv "$TARGET.tmp/gradle-$VERSION"/* "$TARGET/"
    rm -rf "$TARGET.tmp" "$TMP"
fi

exec "$TARGET/bin/gradle" "$@"
