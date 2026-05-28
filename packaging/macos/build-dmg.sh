#!/usr/bin/env bash
# Build snapx.app and .dmg on macOS.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:-$(grep -m1 'project(snapx VERSION' "${ROOT}/CMakeLists.txt" | sed 's/.*VERSION \([0-9.]*\).*/\1/')}"
BUILD="${ROOT}/build"
APP="${ROOT}/dist/snapx.app"
OUT="${ROOT}/packaging/macos/output"
DMG="${OUT}/snapx-${VERSION}-macos.dmg"

cd "$ROOT"
bash packaging/icons/generate-icons.sh 2>/dev/null || true

cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

rm -rf "$APP"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"

cp "${BUILD}/snapx" "${APP}/Contents/MacOS/snapx"
chmod +x "${APP}/Contents/MacOS/snapx"

cp "${ROOT}/packaging/macos/Info.plist" "${APP}/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${VERSION}" "${APP}/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion ${VERSION}" "${APP}/Contents/Info.plist" 2>/dev/null || true

ICON="${ROOT}/resources/icons/hicolor/256x256/apps/snapx.png"
if [[ -f "$ICON" ]]; then
    mkdir -p "${APP}/Contents/Resources"
    sips -z 512 512 "$ICON" --out "${APP}/Contents/Resources/AppIcon.png" 2>/dev/null || \
        cp "$ICON" "${APP}/Contents/Resources/AppIcon.png"
    /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile AppIcon" "${APP}/Contents/Info.plist" 2>/dev/null || true
fi

mkdir -p "$OUT"
rm -f "$DMG"
hdiutil create -volname "snapx" -srcfolder "$APP" -ov -format UDZO "$DMG"

echo "Built: ${DMG}"
