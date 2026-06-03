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

# Bundle the runtime from the current MSYS2 prefix.
#
# A GTK4 app pulls in dozens of DLLs (GLib, Pango, HarfBuzz, FreeType, libpng,
# libjpeg, libwebp, libcurl + its TLS stack, libwinpthread, …).  Hand-listing
# them is fragile and was the cause of "libcurl-4.dll not found" at launch, so
# we resolve the *full transitive closure* from the linked binary with ldd and
# copy every dependency that lives inside the MSYS2 prefix (Windows system DLLs
# under C:\Windows are left to the OS).
if [[ -n "${MINGW_PREFIX:-}" ]]; then
    BIN="${MINGW_PREFIX}/bin"

    copied=0
    if command -v ldd >/dev/null 2>&1; then
        echo "Collecting dependent DLLs via ldd…"
        # ldd recurses over the whole import tree.  We key off the dependency
        # *name* (column 1) and copy it from the MSYS2 prefix whenever it exists
        # there.  This is more robust than filtering by ldd's resolved path:
        # some prefix-provided DLLs (notably vulkan-1.dll, which GTK4 imports)
        # are shadowed by a copy in C:\Windows\System32 on the build runner, so
        # a path filter would wrongly skip them — yet target machines may lack
        # them.  Copying by name from the prefix bundles those while still
        # excluding genuine OS DLLs (kernel32.dll, api-ms-win-*) that aren't in
        # the prefix at all.
        while read -r name; do
            if [[ -n "$name" && -f "${BIN}/${name}" ]]; then
                cp -u "${BIN}/${name}" "${BUILD}/" && copied=$((copied + 1))
            fi
        done < <(ldd "${BUILD}/snapx.exe" | awk '{print $1}' | sort -u)
        echo "Bundled ${copied} DLLs from ${MINGW_PREFIX}."
    fi

    # Explicit extras that ldd may miss (dynamically loaded) or that get
    # shadowed by System32 — copy from the prefix when present.
    for extra in vulkan-1.dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
        [[ -f "${BIN}/${extra}" ]] && cp -u "${BIN}/${extra}" "${BUILD}/"
    done

    # Fallback / belt-and-suspenders: ensure the core stack is present even if
    # ldd is unavailable or missed something.
    if [[ "$copied" -eq 0 ]]; then
        echo "ldd unavailable — copying a curated DLL set."
        for dll in libgtk-4-1.dll libcairo-2.dll libcairo-gobject-2.dll \
                   libgdk_pixbuf-2.0-0.dll libglib-2.0-0.dll libgobject-2.0-0.dll \
                   libgio-2.0-0.dll libgmodule-2.0-0.dll libpango-1.0-0.dll \
                   libpangocairo-1.0-0.dll libpangowin32-1.0-0.dll \
                   libharfbuzz-0.dll libfreetype-6.dll libfontconfig-1.dll \
                   libpng16-16.dll libjpeg-8.dll libwebp-7.dll libwebpdemux-2.dll \
                   libcurl-4.dll libcrypto-3-x64.dll libssl-3-x64.dll \
                   libgraphene-1.0-0.dll libepoxy-0.dll libpixman-1-0.dll \
                   libfribidi-0.dll libffi-8.dll libintl-8.dll libiconv-2.dll \
                   libpcre2-8-0.dll zlib1.dll libwinpthread-1.dll \
                   libgcc_s_seh-1.dll libstdc++-6.dll libtiff-6.dll \
                   libtesseract-5.dll liblept-5.dll libzstd.dll \
                   libbrotlidec.dll libbrotlicommon.dll libssh2-1.dll \
                   libnghttp2-14.dll libidn2-0.dll libunistring-5.dll; do
            [[ -f "${BIN}/${dll}" ]] && cp "${BIN}/${dll}" "${BUILD}/"
        done
    fi

    # gdk-pixbuf image loaders (PNG/JPEG/etc.) + a relocated loader cache.
    if [[ -d "${MINGW_PREFIX}/lib/gdk-pixbuf-2.0" ]]; then
        mkdir -p "${BUILD}/lib"
        cp -r "${MINGW_PREFIX}/lib/gdk-pixbuf-2.0" "${BUILD}/lib/"
        # The cache shipped in the prefix hard-codes the build machine's loader
        # paths (…/ucrt64/lib/gdk-pixbuf-2.0/…), which don't exist on the user's
        # PC, so PNG/SVG decoding and stock icons silently fail.  Rewrite it to
        # reference loaders by bare filename; snapx points GDK_PIXBUF_MODULE_FILE
        # at this copy and the loaders sit beside it, so relative lookup works.
        cache="${BUILD}/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
        if [[ -f "$cache" ]]; then
            sed -i -E 's#^"([^"]*[/\\])?([^"/\\]+\.dll)"#"\2"#' "$cache"
        fi
    fi

    # GLib settings schemas + GTK data; recompile schemas so GSettings works.
    mkdir -p "${BUILD}/share"
    cp -r "${MINGW_PREFIX}/share/glib-2.0" "${BUILD}/share/" 2>/dev/null || true
    cp -r "${MINGW_PREFIX}/share/gtk-4.0"  "${BUILD}/share/" 2>/dev/null || true
    if command -v glib-compile-schemas >/dev/null 2>&1; then
        glib-compile-schemas "${BUILD}/share/glib-2.0/schemas" 2>/dev/null || true
    fi

    # Icon themes so GTK's symbolic/stock icons render.
    mkdir -p "${BUILD}/share/icons"
    cp -r "${MINGW_PREFIX}/share/icons/Adwaita" "${BUILD}/share/icons/" 2>/dev/null || true
    cp -r "${MINGW_PREFIX}/share/icons/hicolor" "${BUILD}/share/icons/" 2>/dev/null || true
fi

mkdir -p "$OUT"
ISCC="${ISCC:-ISCC.exe}"
ISS="${ROOT}/packaging/windows/installer.iss"

if [[ "${SKIP_ISCC:-0}" == "1" ]]; then
    echo "SKIP_ISCC=1 — binary ready in ${BUILD}; run ISCC separately."
    exit 0
fi

"${ISCC}" "/DMyAppVersion=${VERSION}" "/DMyBuildDir=${BUILD}" "${ISS}"

ls -la "${OUT}/"
