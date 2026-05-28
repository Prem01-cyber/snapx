#!/usr/bin/env bash
# Build snapx + Inno Setup installer (MSYS2 UCRT64 shell).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:-$(grep -m1 'project(snapx VERSION' "${ROOT}/CMakeLists.txt" | sed 's/.*VERSION \([0-9.]*\).*/\1/')}"
BUILD="${ROOT}/build"
OUT="${ROOT}/packaging/windows/output"

cd "$ROOT"
bash packaging/icons/generate-icons.sh 2>/dev/null || true

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD"

# Bundle DLLs from current MSYS prefix
if [[ -n "${MINGW_PREFIX:-}" ]]; then
    BIN="${MINGW_PREFIX}/bin"
    for dll in libgtk-4-1.dll libcairo-2.dll libgdk_pixbuf-2.0-0.dll \
               libglib-2.0-0.dll libgobject-2.0-0.dll libgio-2.0-0.dll; do
        [[ -f "${BIN}/${dll}" ]] && cp "${BIN}/${dll}" "${BUILD}/"
    done
    mkdir -p "${BUILD}/share"
    cp -r "${MINGW_PREFIX}/share/glib-2.0" "${BUILD}/share/" 2>/dev/null || true
    cp -r "${MINGW_PREFIX}/share/gtk-4.0" "${BUILD}/share/" 2>/dev/null || true
fi

mkdir -p "$OUT"
ISCC="${ISCC:-iscc}"
"$ISCC" "/DMyAppVersion=${VERSION}" "/DMyBuildDir=${BUILD}" \
    "${ROOT}/packaging/windows/installer.iss"

ls -la "${OUT}/"
