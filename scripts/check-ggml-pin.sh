#!/usr/bin/env bash
# Check that the standalone ggml pin (packaging/vendor/ggml.conf) is the
# ggml revision the llama.cpp pin (packaging/vendor/llama.cpp.conf) last
# synced, as recorded in llama.cpp's scripts/sync-ggml.last. Keeping the two
# together means a ggml fix or a new GPU backend reaches onyx-engine and the
# BackTrack audio apps at the same time.
#
# Usage: scripts/check-ggml-pin.sh [path-to-another-ggml.conf]
# Exits 1 when the pins have drifted, printing the commit to move to.
set -euo pipefail

cd "$(dirname "$0")/.."
. packaging/vendor/lib.sh

load_vendor_spec llama.cpp
llama_upstream="$VENDOR_UPSTREAM"
llama_commit="$VENDOR_COMMIT"
load_vendor_spec "${1:-ggml}"
ggml_commit="$VENDOR_COMMIT"
ggml_file="$VENDOR_SPEC_FILE"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git init -q "$tmp"
git -C "$tmp" remote add origin "$llama_upstream"
# Tree only; the one blob we read is fetched on demand.
git -C "$tmp" fetch -q --depth 1 --filter=blob:none origin "$llama_commit"
synced=$(git -C "$tmp" show FETCH_HEAD:scripts/sync-ggml.last | tr -d '[:space:]')

if [ "$synced" = "$ggml_commit" ]; then
    echo "ok: $ggml_file pins ggml $ggml_commit, the revision llama.cpp ${llama_commit:0:9} synced"
else
    echo "drift: $ggml_file pins ggml $ggml_commit" >&2
    echo "       llama.cpp ${llama_commit:0:9} synced ggml $synced; move VENDOR_COMMIT there" >&2
    exit 1
fi
