#!/usr/bin/env bash
# Build snapx AppImage and optional portable tarball.
# Usage: ./packaging/linux/build-appimage.sh [version]
# Requires: cmake, linuxdeploy, linuxdeploy-plugin-gtk, appimagetool (or wget them)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:-$(grep -m1 'project(snapx VERSION' "${ROOT}/CMakeLists.txt" | sed 's/.*VERSION \([0-9.]*\).*/\1/')}"
BUILD="${ROOT}/build"
APPDIR="${ROOT}/dist/AppDir"
OUT="${ROOT}/packaging/linux/output"
ARCH="$(uname -m)"

cd "$ROOT"
"${ROOT}/packaging/icons/generate-icons.sh" 2>/dev/null || true

cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)"

rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" --prefix /usr

# AppRun + desktop at AppDir root for AppImage
install -Dm755 "${APPDIR}/usr/bin/snapx" "${APPDIR}/usr/bin/snapx"
cp "${APPDIR}/usr/share/applications/snapx.desktop" "${APPDIR}/snapx.desktop" 2>/dev/null || \
    cp "${ROOT}/packaging/linux/snapx.desktop" "${APPDIR}/snapx.desktop"
sed -i 's|Exec=snapx|Exec=snapx|g' "${APPDIR}/snapx.desktop" 2>/dev/null || true

ICON="${ROOT}/resources/icons/hicolor/256x256/apps/snapx.png"
[[ -f "$ICON" ]] && cp "$ICON" "${APPDIR}/snapx.png"

LINUXDEPLOY="${LINUXDEPLOY:-linuxdeploy}"
PLUGIN_GTK="${LINUXDEPLOY_PLUGIN_GTK:-linuxdeploy-plugin-gtk}"
APPIMAGETOOL="${APPIMAGETOOL:-appimagetool}"

if ! command -v "$LINUXDEPLOY" >/dev/null 2>&1; then
    echo "Downloading linuxdeploy..."
    wget -q -O /tmp/linuxdeploy-"${ARCH}".AppImage \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-"${ARCH}".AppImage"
    chmod +x /tmp/linuxdeploy-"${ARCH}".AppImage
    LINUXDEPLOY="/tmp/linuxdeploy-${ARCH}.AppImage"
fi

if ! command -v "$PLUGIN_GTK" >/dev/null 2>&1; then
    wget -q -O /tmp/linuxdeploy-plugin-gtk-"${ARCH}".AppImage \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-gtk/releases/download/continuous/linuxdeploy-plugin-gtk-"${ARCH}".AppImage" || true
    chmod +x /tmp/linuxdeploy-plugin-gtk-"${ARCH}.AppImage" 2>/dev/null || true
    PLUGIN_GTK="/tmp/linuxdeploy-plugin-gtk-${ARCH}.AppImage"
fi

export DEPLOY_GTK_VERSION=4
"$LINUXDEPLOY" --appdir "$APPDIR" --executable "${APPDIR}/usr/bin/snapx" \
    --desktop-file "${APPDIR}/snapx.desktop" \
    --icon-file "${APPDIR}/snapx.png" \
    --plugin gtk || "$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "${APPDIR}/usr/bin/snapx" \
    --desktop-file "${APPDIR}/snapx.desktop" \
    --icon-file "${APPDIR}/snapx.png"

mkdir -p "$OUT"
APPIMAGE_NAME="snapx-${VERSION}-${ARCH}.AppImage"

if command -v "$APPIMAGETOOL" >/dev/null 2>&1; then
    ARCH="$ARCH" "$APPIMAGETOOL" "$APPDIR" "${OUT}/${APPIMAGE_NAME}"
elif [[ -x /tmp/appimagetool-"${ARCH}".AppImage ]]; then
    ARCH="$ARCH" /tmp/appimagetool-"${ARCH}".AppImage "$APPDIR" "${OUT}/${APPIMAGE_NAME}"
else
    wget -q -O /tmp/appimagetool-"${ARCH}".AppImage \
        "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-"${ARCH}".AppImage"
    chmod +x /tmp/appimagetool-"${ARCH}".AppImage
    ARCH="$ARCH" /tmp/appimagetool-"${ARCH}".AppImage "$APPDIR" "${OUT}/${APPIMAGE_NAME}"
fi

chmod +x "${OUT}/${APPIMAGE_NAME}"

TARBALL="${OUT}/snapx-${VERSION}-linux-${ARCH}.tar.gz"
tar -C "${APPDIR}/usr" -czf "$TARBALL" bin share 2>/dev/null || \
    tar -C "$APPDIR" -czf "$TARBALL" usr

echo "Built:"
echo "  ${OUT}/${APPIMAGE_NAME}"
echo "  ${TARBALL}"
