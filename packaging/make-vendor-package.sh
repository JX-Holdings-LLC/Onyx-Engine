#!/usr/bin/env bash
# Builds the @jxburros/llama-cpp-source npm package: the pinned llama.cpp
# source tree, pruned to what jx-engine's build and test tooling need.
#
# This is the MAINTAINER packaging step (run once per llama.cpp upgrade, then
# `npm publish` the tarball). Consumers of jx-engine never run this; their
# only external build input is the published npm package.
#
# Usage:
#   packaging/make-vendor-package.sh [path-to-llama.cpp-source]
#
# Without an argument the pinned commit is downloaded from GitHub (network
# access required for packaging only, not for building jx-engine).
set -euo pipefail

cd "$(dirname "$0")/.."

# ---- pin -------------------------------------------------------------------
LLAMA_COMMIT="9723942adc51ec2f2b7c9dcc86842934c479b336"
LLAMA_VERSION="0.3.0"                 # llama.cpp's own CMake project version
PKG_NAME="@jxburros/llama-cpp-source"
PKG_VERSION="${LLAMA_VERSION}-b10711.g${LLAMA_COMMIT:0:9}"
# ----------------------------------------------------------------------------

SRC="${1:-}"
DIST="packaging/dist"
STAGE="$DIST/package"

if [ -z "$SRC" ]; then
    SRC="$DIST/llama.cpp-$LLAMA_COMMIT"
    if [ ! -d "$SRC" ]; then
        echo "downloading llama.cpp @ $LLAMA_COMMIT ..."
        mkdir -p "$DIST"
        curl -fsSL "https://github.com/ggml-org/llama.cpp/archive/$LLAMA_COMMIT.tar.gz" \
            | tar -xz -C "$DIST"
    fi
fi
[ -f "$SRC/CMakeLists.txt" ] || { echo "error: '$SRC' is not a llama.cpp source tree"; exit 1; }

echo "staging from $SRC ..."
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Everything the library build + jx-engine test tooling need. Deliberately
# excluded: docs, tests, examples, benches, media, pocs, ci, app, conversion
# scripts and the bulk of models/ (only the vocab file our tiny-model
# generator reads is kept).
cp -a "$SRC/." "$STAGE/"
rm -rf "$STAGE/.git" "$STAGE/.github" "$STAGE/.devops" \
       "$STAGE/docs" "$STAGE/tests" "$STAGE/examples" "$STAGE/benches" \
       "$STAGE/media" "$STAGE/pocs" "$STAGE/ci" "$STAGE/app" \
       "$STAGE/conversion" "$STAGE/requirements" "$STAGE/requirements.txt" \
       "$STAGE/models"

mkdir -p "$STAGE/models"
cp "$SRC/models/ggml-vocab-llama-spm.gguf" "$STAGE/models/"

# npm pack honors .gitignore files unless .npmignore exists; llama.cpp's
# .gitignore would silently drop files (e.g. *.gguf), so remove them and ship
# an explicit empty-ish .npmignore instead.
find "$STAGE" -name '.gitignore' -delete
printf '# intentionally minimal - the staging script already pruned the tree\n' > "$STAGE/.npmignore"

cat > "$STAGE/package.json" <<EOF
{
  "name": "$PKG_NAME",
  "version": "$PKG_VERSION",
  "description": "Pinned, pruned llama.cpp source tree consumed by jx-engine's CMake build. Not a Node.js library - contains C/C++ sources only.",
  "license": "MIT",
  "homepage": "https://github.com/ggml-org/llama.cpp",
  "repository": {
    "type": "git",
    "url": "git+https://github.com/JX-Holdings-LLC/JX-Engine.git",
    "directory": "packaging"
  },
  "llamaCppCommit": "$LLAMA_COMMIT",
  "llamaCppUpstream": "https://github.com/ggml-org/llama.cpp"
}
EOF

echo "packing ..."
( cd "$DIST" && npm pack ./package --silent )

TARBALL=$(ls "$DIST"/jxburros-llama-cpp-source-*.tgz | tail -1)
echo
echo "wrote  $TARBALL"
du -sh "$STAGE" "$TARBALL" | sed 's/^/  /'
echo
echo "to publish:  npm publish $TARBALL --access public"
