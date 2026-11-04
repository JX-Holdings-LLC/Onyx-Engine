#!/usr/bin/env bash
# Check that a build carries no ggml RPC backend. Its server runs tensor
# operations for any peer that connects, and upstream's remote-code-execution
# advisory (GHSA-j8rj-fmpv-wcxw) lists no patched version, so onyx-engine never
# builds it. CMakeLists.txt forces GGML_RPC off and fails the configure if a
# ggml-rpc target exists; this checks the result, so a pin bump that moves the
# backend somewhere the CMake check does not see still fails CI.
#
# Usage: scripts/check-no-rpc.sh [build-dir]     (default: build)
# Exits 1 naming each offending file or setting.
set -euo pipefail

dir="${1:-build}"
[ -f "$dir/CMakeCache.txt" ] || { echo "error: '$dir' is not a configured build directory" >&2; exit 1; }

bad=0
if ! grep -q '^GGML_RPC:BOOL=OFF$' "$dir/CMakeCache.txt"; then
    echo "rpc: $dir/CMakeCache.txt does not have GGML_RPC=OFF" >&2
    bad=1
fi
# The binaries and libraries a build produces. Their names differ by platform;
# the functions ggml-rpc defines do not. (ggml_backend_rpc_add_server is left
# out: common/arg.cpp names it as a string, to look it up at run time.)
while IFS= read -r -d '' f; do
    if grep -q -a -E 'ggml_backend_rpc_(init|start_server|get_device_memory)' "$f"; then
        echo "rpc: $f contains the ggml RPC backend" >&2
        bad=1
    fi
done < <(find "$dir" -type f \( -name 'onyx-engine' -o -name 'onyx-engine.exe' -o -name '*.so' -o -name '*.so.*' \
    -o -name '*.dylib' -o -name '*.dll' -o -name '*.a' -o -name '*.lib' \) -print0)
if find "$dir" -name '*ggml-rpc*' -not -path '*/CMakeFiles/*' | grep -q .; then
    echo "rpc: $dir has ggml-rpc build outputs:" >&2
    find "$dir" -name '*ggml-rpc*' -not -path '*/CMakeFiles/*' >&2
    bad=1
fi
[ "$bad" = 0 ] || exit 1
echo "ok: no ggml RPC backend in $dir"
