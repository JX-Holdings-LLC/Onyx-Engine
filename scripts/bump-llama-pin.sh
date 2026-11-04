#!/usr/bin/env bash
# Move the llama.cpp pin (packaging/vendor/llama.cpp.conf) to a newer upstream
# release, and the ggml pin (packaging/vendor/ggml.conf) to the revision that
# release synced, and write what a reviewer needs to judge the move: the diff
# of every upstream header Onyx compiles against (common/ plus llama.h,
# llama-cpp.h, llama-ext.h, mtmd.h, mtmd-helper.h) and a pull-request body.
# .github/workflows/pin-bump.yml runs it on a schedule and opens the PR.
#
#   scripts/bump-llama-pin.sh --dry-run             # latest release; change nothing
#   scripts/bump-llama-pin.sh                       # latest release; edit the pins
#   scripts/bump-llama-pin.sh --build b12111        # a given release
#   scripts/bump-llama-pin.sh --out DIR             # where the diff and body go
#
# Without --dry-run it edits both specs and, through the pinned release
# toolkit at .toolkit (see docs/development/releasing.md), advances
# vendor-llama-cpp, vendor-ggml (when its pin moves) and onyx-engine (patch),
# writing their release notes and a changelog entry. It never commits. Exits 0
# with "up to date" when the pin is already at or past the target. Needs Bash,
# Git and network access to GitHub; Node only without --dry-run.
set -euo pipefail

cd "$(dirname "$0")/.."
root=$PWD
. packaging/vendor/lib.sh

TARGET=latest
OUT=""
DRY_RUN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build) TARGET="${2:?--build needs a value}"; shift 2 ;;
        --build=*) TARGET="${1#--build=}"; shift ;;
        --out) OUT="${2:?--out needs a value}"; shift 2 ;;
        --out=*) OUT="${1#--out=}"; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

# key=value for the workflow; a no-op outside Actions.
output() { [ -z "${GITHUB_OUTPUT:-}" ] || echo "$1=$2" >> "$GITHUB_OUTPUT"; }

load_vendor_spec llama.cpp
upstream="$VENDOR_UPSTREAM"
old_commit="$VENDOR_COMMIT"
old_build="$VENDOR_BUILD"
old_llama_pkg="$PKG_VERSION"
load_vendor_spec ggml
ggml_upstream="$VENDOR_UPSTREAM"
old_ggml="$VENDOR_COMMIT"
old_ggml_pkg="$PKG_VERSION"

# The target release: the highest bNNNN tag, or the one asked for.
case "$TARGET" in
    latest)
        build=$(git ls-remote --tags "$upstream" 'refs/tags/b*' |
            sed -n 's#^[0-9a-f]*[[:space:]]refs/tags/b\([0-9][0-9]*\)$#\1#p' | sort -n | tail -1)
        [ -n "$build" ] || { echo "error: no bNNNN release tags at $upstream" >&2; exit 1; } ;;
    b[0-9]*) build="${TARGET#b}" ;;
    [0-9]*) build="$TARGET" ;;
    *) echo "error: --build takes bNNNN or latest, not '$TARGET'" >&2; exit 2 ;;
esac
[[ "$build" =~ ^[0-9]+$ ]] || { echo "error: bad build '$TARGET'" >&2; exit 2; }

output build "b$build"
if [ "$build" -le "$old_build" ]; then
    echo "up to date: llama.cpp is pinned at b$old_build (${old_commit:0:9}); target b$build"
    output changed false
    exit 0
fi

# A tag may be lightweight or annotated; take the commit it peels to.
refs=$(git ls-remote "$upstream" "refs/tags/b$build" "refs/tags/b$build^{}")
commit=$(printf '%s\n' "$refs" | awk '$2 ~ /\^\{\}$/ {print $1}')
[ -n "$commit" ] || commit=$(printf '%s\n' "$refs" | awk 'NR==1 {print $1}')
[[ "$commit" =~ ^[0-9a-f]{40}$ ]] || { echo "error: no tag b$build at $upstream" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git init -q "$tmp/llama"
git -C "$tmp/llama" remote add origin "$upstream"
# Trees only; the blobs read below are fetched on demand.
git -C "$tmp/llama" fetch -q --depth 1 --filter=blob:none origin "$old_commit" "$commit"

# The version a CMakeLists.txt declares as <PREFIX>_VERSION_{MAJOR,MINOR,PATCH}.
cmake_version() { # repo commit prefix
    local text v part
    text=$(git -C "$1" show "$2:CMakeLists.txt")
    v=""
    for part in MAJOR MINOR PATCH; do
        local n
        n=$(printf '%s\n' "$text" | sed -n "s/^set(${3}_VERSION_${part} \([0-9][0-9]*\))/\1/p" | head -1)
        [ -n "$n" ] || { echo "error: no ${3}_VERSION_${part} in $2:CMakeLists.txt" >&2; return 1; }
        v="${v:+$v.}$n"
    done
    echo "$v"
}

llama_version=$(cmake_version "$tmp/llama" "$commit" LLAMA)
ggml=$(git -C "$tmp/llama" show "$commit:scripts/sync-ggml.last" | tr -d '[:space:]')
[[ "$ggml" =~ ^[0-9a-f]{40}$ ]] || { echo "error: b$build has no usable scripts/sync-ggml.last" >&2; exit 1; }
llama_pkg="$llama_version-b$build.g${commit:0:9}"

if [ "$ggml" = "$old_ggml" ]; then
    ggml_version=""
    ggml_pkg="$old_ggml_pkg"
else
    git init -q "$tmp/ggml"
    git -C "$tmp/ggml" remote add origin "$ggml_upstream"
    git -C "$tmp/ggml" fetch -q --depth 1 --filter=blob:none origin "$ggml"
    ggml_version=$(cmake_version "$tmp/ggml" "$ggml" GGML)
    ggml_pkg="$ggml_version-g${ggml:0:9}"
fi

# Every upstream header Onyx includes: all of common/ (chat.h, common.h,
# jinja/, ...), libllama's public and staging headers, and mtmd's.
headers=(':(glob)common/**/*.h' include/llama.h include/llama-cpp.h src/llama-ext.h
         tools/mtmd/mtmd.h tools/mtmd/mtmd-helper.h)
OUT="${OUT:-$root/packaging/dist/pin-bump}"
mkdir -p "$OUT"
git -C "$tmp/llama" diff "$old_commit" "$commit" -- "${headers[@]}" > "$OUT/headers.diff"
header_stat=$(git -C "$tmp/llama" diff --stat=100 "$old_commit" "$commit" -- "${headers[@]}")
common_files=$(git -C "$tmp/llama" diff --name-status "$old_commit" "$commit" -- common)
# Files outside the specs and release metadata that name the old pin in prose.
mentions=$(git grep -l -e "b$old_build" -e "${old_commit:0:9}" -- . \
    ':!CHANGELOG.md' ':!.release/notes' ':!packaging/vendor' ':!release.json' || true)

echo "llama.cpp: b$old_build (${old_commit:0:9}) -> b$build (${commit:0:9}), package $llama_pkg"
if [ -n "$ggml_version" ]; then
    echo "ggml:      ${old_ggml:0:7} -> ${ggml:0:7}, package $ggml_pkg"
else
    echo "ggml:      unchanged at ${old_ggml:0:7}"
fi
echo "headers:   $(printf '%s\n' "$header_stat" | tail -1 | sed 's/^ *//')"
echo "           full diff in $OUT/headers.diff"

# A pin bump is a patch release (docs/reference/stability.md) unless the
# reviewer finds a stable surface changed.
onyx_old=$(sed -n 's/^  "version": "\(.*\)",$/\1/p' package.json | head -1)
[[ "$onyx_old" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || { echo "error: package.json version '$onyx_old'" >&2; exit 1; }
onyx_new="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.$((BASH_REMATCH[3] + 1))"
today=$(date -u +%Y-%m-%d)

if [ "$DRY_RUN" = 0 ]; then
    [ -f .toolkit/src/release.mjs ] || { echo "error: the release toolkit is not at .toolkit (see docs/development/releasing.md)" >&2; exit 1; }
    # The real path: release.mjs runs its CLI only when argv[1] is its own file.
    toolkit=$(cd .toolkit/src && pwd -P)/release.mjs
    bump() { # app version
        node "$toolkit" bump --app "$1" --version "$2"
        grep -q "\"version\": \"$2\"" release.json || { echo "error: release.mjs did not advance $1 to $2" >&2; exit 1; }
    }
    set_key() { # file key value
        sed -i.bak "s|^$2=\".*\"\$|$2=\"$3\"|" "$1" && rm -f "$1.bak"
        grep -q "^$2=\"$3\"\$" "$1" || { echo "error: could not set $2 in $1" >&2; exit 1; }
    }
    f=packaging/vendor/llama.cpp.conf
    set_key "$f" VENDOR_COMMIT "$commit"
    set_key "$f" VENDOR_UPSTREAM_VERSION "$llama_version"
    set_key "$f" VENDOR_BUILD "$build"
    bump vendor-llama-cpp "$llama_pkg"
    if [ -n "$ggml_version" ]; then
        f=packaging/vendor/ggml.conf
        set_key "$f" VENDOR_COMMIT "$ggml"
        set_key "$f" VENDOR_UPSTREAM_VERSION "$ggml_version"
        bump vendor-ggml "$ggml_pkg"
    fi
    bump onyx-engine "$onyx_new"

    printf '# @jxburros/llama-cpp-source %s\n\n## Changes\n\nllama.cpp b%s (`%s`), up from b%s (`%s`).\n' \
        "$llama_pkg" "$build" "${commit:0:9}" "$old_build" "${old_commit:0:9}" \
        > ".release/notes/vendor-llama-cpp/$llama_pkg.md"
    if [ -n "$ggml_version" ]; then
        printf '# @jxburros/ggml-source %s\n\n## Changes\n\nggml `%s`, the revision llama.cpp b%s synced, up from `%s`.\n' \
            "$ggml_pkg" "${ggml:0:7}" "$build" "${old_ggml:0:7}" \
            > ".release/notes/vendor-ggml/$ggml_pkg.md"
    fi
    printf '# Onyx Engine %s\n\n## Changes\n\n- llama.cpp moves from b%s to b%s.\n' \
        "$onyx_new" "$old_build" "$build" > ".release/notes/onyx-engine/$onyx_new.md"

    # A changelog entry under its own version heading, below [Unreleased].
    entry=$(cat <<EOF
## [$onyx_new] — $today

### llama.cpp b$build — $today ($onyx_new)

Agent: pin-bump workflow (\`scripts/bump-llama-pin.sh\`). Pin moves only; a reviewer completes this
entry with the code changes the bump needed.

- **llama.cpp b$old_build → b$build** (\`${old_commit:0:9}\` → \`${commit:0:9}\`); vendor package
  \`$llama_pkg\`.
EOF
)
    if [ -n "$ggml_version" ]; then
        entry+=$'\n'"- **ggml \`${old_ggml:0:7}\` → \`${ggml:0:7}\`**, the revision b$build synced; vendor package"
        entry+=$'\n'"  \`$ggml_pkg\`."
    fi
    ENTRY="$entry" awk '{ print } /^## \[Unreleased\]$/ && !done { print ""; print ENVIRON["ENTRY"]; done = 1 }
        END { exit !done }' CHANGELOG.md > CHANGELOG.md.new ||
        { rm -f CHANGELOG.md.new; echo "error: CHANGELOG.md has no '## [Unreleased]' heading" >&2; exit 1; }
    mv CHANGELOG.md.new CHANGELOG.md

    bash scripts/check-ggml-pin.sh
fi

# The pull-request body. GitHub caps a body at 65536 characters, so the
# inline diff is cut short; the workflow uploads the whole file.
{
    echo "Moves llama.cpp from **b$old_build** (\`${old_commit:0:9}\`) to **b$build** (\`${commit:0:9}\`):"
    echo "[upstream changes]($upstream/compare/b$old_build...b$build)."
    if [ -n "$ggml_version" ]; then
        echo "ggml moves from \`${old_ggml:0:7}\` to \`${ggml:0:7}\`, the revision b$build synced"
        echo "([changes]($ggml_upstream/compare/${old_ggml:0:12}...${ggml:0:12}))."
    else
        echo "The ggml pin stays at \`${old_ggml:0:7}\`: b$build synced the same revision."
    fi
    echo
    echo "Generated by \`scripts/bump-llama-pin.sh\`. Versions: onyx-engine $onyx_new,"
    echo "vendor-llama-cpp \`$llama_pkg\` (was \`$old_llama_pkg\`), vendor-ggml \`$ggml_pkg\`."
    echo
    echo "## Before merging"
    echo
    echo "- [ ] CI builds and passes the unit and smoke tests on all three platforms; fix what the"
    echo "      header diff below broke, in this branch."
    echo "- [ ] Complete the changelog entry and \`.release/notes/onyx-engine/\` with those fixes."
    echo "- [ ] Check the files that name the old pin (a provenance note may rightly keep it):"
    if [ -n "$mentions" ]; then
        printf '%s\n' "$mentions" | sed 's/^/      `/; s/$/`/'
    else
        echo "      (none found)"
    fi
    echo "- [ ] Move BackTrack's ggml pin with \`scripts/check-ggml-pin.sh ../BackTrack/packaging/vendor/ggml.conf\`."
    echo
    echo "## Upstream headers Onyx compiles against"
    echo
    echo '```'
    printf '%s\n' "$header_stat"
    echo '```'
    echo
    echo "<details><summary>Files changed under <code>common/</code></summary>"
    echo
    echo '```'
    printf '%s\n' "${common_files:-(none)}"
    echo '```'
    echo
    echo "</details>"
    echo
    echo "<details><summary>Header diff</summary>"
    echo
    echo '```diff'
    head -c 45000 "$OUT/headers.diff"
    [ "$(wc -c < "$OUT/headers.diff")" -le 45000 ] || printf '\n... cut short; the whole diff is the workflow run'"'"'s pin-bump artifact\n'
    echo '```'
    echo
    echo "</details>"
} > "$OUT/pr-body.md"
echo "pr body:   $OUT/pr-body.md"

output changed true
output branch "pin-bump/b$build"
output title "Move llama.cpp to b$build"
output onyx_version "$onyx_new"
