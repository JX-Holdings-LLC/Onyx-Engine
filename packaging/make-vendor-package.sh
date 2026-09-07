#!/usr/bin/env bash
# Builds the @jxburros/llama-cpp-source npm package: the pinned llama.cpp
# source tree, pruned to what onyx-engine's build and test tooling need.
#
# This is the MAINTAINER packaging step (run once per llama.cpp upgrade, then
# `npm publish` the tarball). It is also the local fallback a fresh clone
# uses while `@jxburros/llama-cpp-source` is unpublished — `npm run vendor`
# (scripts/vendor.sh) calls this script and installs the resulting tarball
# into node_modules, which is all CMake needs.
#
# Usage:
#   packaging/make-vendor-package.sh [path-to-llama.cpp-source]
#
# Without an argument the pinned commit is fetched from GitHub — first as a
# source archive over HTTPS, and if that is unreachable, as a depth-1 git
# fetch of the exact commit (network access required for packaging only, not
# for building onyx-engine).
set -euo pipefail

cd "$(dirname "$0")/.."

# ---- pin -------------------------------------------------------------------
LLAMA_COMMIT="9723942adc518b43c4b95dc4dce6906903eb5e09"
LLAMA_VERSION="0.3.0"                 # llama.cpp's own CMake project version
PKG_NAME="@jxburros/llama-cpp-source"
PKG_VERSION="${LLAMA_VERSION}-b10711.g${LLAMA_COMMIT:0:9}"
# ----------------------------------------------------------------------------

SRC="${1:-}"
DIST="packaging/dist"
STAGE="$DIST/package"

LLAMA_UPSTREAM="https://github.com/ggml-org/llama.cpp"

# Fetch the pinned commit with git. Used as a fallback when the codeload
# tarball is unreachable — some networks (corporate proxies, the sandboxes
# CI and agents run in) allow `git clone` over HTTPS but return 403 for
# `github.com/.../archive/*.tar.gz`. A depth-1 fetch of the exact commit
# costs about the same as the tarball and pins identically.
fetch_with_git() {
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest"
    git init -q "$dest"
    git -C "$dest" remote add origin "$LLAMA_UPSTREAM"
    git -C "$dest" fetch --depth 1 -q origin "$LLAMA_COMMIT"
    git -C "$dest" checkout -q FETCH_HEAD
    rm -rf "$dest/.git"
}

if [ -z "$SRC" ]; then
    SRC="$DIST/llama.cpp-$LLAMA_COMMIT"
    if [ ! -d "$SRC" ]; then
        mkdir -p "$DIST"
        echo "downloading llama.cpp @ $LLAMA_COMMIT ..."
        # Downloaded to a file rather than piped into tar so a failed fetch
        # reports the HTTP error instead of tar's "unexpected end of file".
        ARCHIVE="$DIST/llama.cpp-$LLAMA_COMMIT.tar.gz"
        if curl -fsSL -o "$ARCHIVE" "$LLAMA_UPSTREAM/archive/$LLAMA_COMMIT.tar.gz" \
           && tar -xzf "$ARCHIVE" -C "$DIST"; then
            rm -f "$ARCHIVE"
        else
            rm -f "$ARCHIVE"
            rm -rf "$SRC"
            echo "archive download unavailable; falling back to git fetch ..."
            command -v git > /dev/null || {
                echo "error: neither the archive download nor git is available" >&2
                exit 1
            }
            fetch_with_git "$SRC"
        fi
    fi
fi
[ -f "$SRC/CMakeLists.txt" ] || { echo "error: '$SRC' is not a llama.cpp source tree"; exit 1; }

echo "staging from $SRC ..."
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Everything the library build + onyx-engine test tooling need. Deliberately
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

# tools/: onyx-engine builds exactly one thing out of this tree - the `mtmd`
# library (multimodal projector support, -DLLAMA_BUILD_MTMD=ON, which
# add_subdirectory()s tools/mtmd directly without going through
# tools/CMakeLists.txt). Everything else under tools/ (server, ui, cli,
# benchmarks, quantize, ...) is never configured, so drop it. tools/mtmd's
# own CMakeLists links vendor::hash, vendor::miniaudio, vendor::stb and
# vendor::sheredom, all of which live under vendor/ and are kept.
find "$STAGE/tools" -mindepth 1 -maxdepth 1 ! -name mtmd -exec rm -rf {} +

# npm pack honors .gitignore files unless .npmignore exists; llama.cpp's
# .gitignore would silently drop files (e.g. *.gguf), so remove them and ship
# an explicit empty-ish .npmignore instead.
find "$STAGE" -name '.gitignore' -delete
printf '# intentionally minimal - the staging script already pruned the tree\n' > "$STAGE/.npmignore"

cat > "$STAGE/package.json" <<EOF
{
  "name": "$PKG_NAME",
  "version": "$PKG_VERSION",
  "description": "Pinned, pruned llama.cpp source tree consumed by onyx-engine's CMake build. Not a Node.js library - contains C/C++ sources only.",
  "license": "MIT",
  "homepage": "https://github.com/ggml-org/llama.cpp",
  "repository": {
    "type": "git",
    "url": "git+https://github.com/JX-Holdings-LLC/Onyx-Engine.git",
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
