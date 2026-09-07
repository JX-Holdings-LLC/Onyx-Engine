#!/usr/bin/env bash
# One-step local fallback for the unpublished @jxburros/llama-cpp-source
# package: stage the pinned llama.cpp vendor tarball and install it where
# CMake looks for it (node_modules/@jxburros/llama-cpp-source).
#
# Run this once after a fresh clone, then build normally:
#
#   npm run vendor      # or: bash scripts/vendor.sh
#   npm run build
#
# Usage:
#   scripts/vendor.sh [path-to-llama.cpp-source]
#
# With no argument the pinned commit is fetched from GitHub by
# packaging/make-vendor-package.sh (source archive, falling back to a
# depth-1 git fetch). Pass a path to reuse a llama.cpp checkout you already
# have — nothing is downloaded in that case.
#
# This does the same thing as the documented manual two-liner and the
# equivalent steps in .github/workflows/{ci,release}.yml: extraction is done
# with `tar` rather than `npm install <tarball>` so the only tools required
# are bash and tar, and the on-disk layout is identical either way.
set -euo pipefail

cd "$(dirname "$0")/.."

DEST="node_modules/@jxburros/llama-cpp-source"

bash packaging/make-vendor-package.sh "$@"

TARBALL=$(ls packaging/dist/jxburros-llama-cpp-source-*.tgz | tail -1)

echo "installing $TARBALL -> $DEST ..."
rm -rf "$DEST"
mkdir -p "$DEST"
tar -xzf "$TARBALL" --strip-components=1 -C "$DEST"

[ -f "$DEST/CMakeLists.txt" ] || {
    echo "error: $DEST/CMakeLists.txt missing after extraction" >&2
    exit 1
}

echo
echo "vendored llama.cpp source ready. Next:  npm run build"
