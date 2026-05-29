#!/usr/bin/env bash
# Build Fedora/RHEL RPM from snapx.spec.
# Usage: ./packaging/linux/build-rpm.sh [version]
# Run on Fedora with: dnf install rpm-build cmake gcc gtk4-devel ...
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:-$(grep -m1 'project(snapx VERSION' "${ROOT}/CMakeLists.txt" | sed 's/.*VERSION \([0-9.]*\).*/\1/')}"
OUT="${ROOT}/packaging/linux/output"
SPEC="${ROOT}/packaging/linux/snapx.spec"
TARBALL="snapx-${VERSION}.tar.gz"

cd "$ROOT"
"${ROOT}/packaging/icons/generate-icons.sh" 2>/dev/null || true

mkdir -p "${OUT}"
rm -f "${OUT}/"*.rpm 2>/dev/null || true

# Source tarball for rpmbuild (%autosetup expects snapx-VERSION/)
TARBALL_PATH="/tmp/${TARBALL}"
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$ROOT" archive --format=tar.gz --prefix="snapx-${VERSION}/" \
        -o "$TARBALL_PATH" HEAD
else
    tar -C "$ROOT" -czf "$TARBALL_PATH" \
        --exclude='.git' --exclude='build' --exclude='dist' \
        --transform="s,^,snapx-${VERSION}/," .
fi

mkdir -p "${HOME}/rpmbuild"/{SOURCES,SPECS,BUILD,RPMS,SRPMS}
cp "$TARBALL_PATH" "${HOME}/rpmbuild/SOURCES/"
cp "${SPEC}" "${HOME}/rpmbuild/SPECS/snapx.spec"

rpmbuild -ba "${HOME}/rpmbuild/SPECS/snapx.spec"

find "${HOME}/rpmbuild/RPMS" -name "*.rpm" -exec cp {} "${OUT}/" \;

echo "Built RPM(s) in ${OUT}:"
ls -la "${OUT}/"*.rpm
