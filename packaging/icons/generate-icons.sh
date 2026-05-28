#!/usr/bin/env bash
# Generate FreeDesktop hicolor PNG icons from resources/icons/snapx.svg
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SVG="${ROOT}/resources/icons/snapx.svg"
OUT="${ROOT}/resources/icons/hicolor"

if [[ ! -f "$SVG" ]]; then
    echo "error: missing $SVG" >&2
    exit 1
fi

convert_raster() {
    local size="$1"
    local outdir="${OUT}/${size}x${size}/apps"
    mkdir -p "$outdir"
    local out="${outdir}/snapx.png"
    if command -v rsvg-convert >/dev/null 2>&1; then
        rsvg-convert -w "$size" -h "$size" -o "$out" "$SVG"
    elif command -v inkscape >/dev/null 2>&1; then
        inkscape -w "$size" -h "$size" -o "$out" "$SVG"
    elif command -v convert >/dev/null 2>&1; then
        convert -background none -resize "${size}x${size}" "$SVG" "$out"
    else
        echo "error: install rsvg-convert, inkscape, or ImageMagick convert" >&2
        exit 1
    fi
    echo "  ${out}"
}

echo "Generating icons from ${SVG}..."
for size in 16 32 48 64 128 256; do
    convert_raster "$size"
done
echo "Done."
