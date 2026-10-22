#!/bin/zsh
# Usage: scripts/dxmt-patch.sh [build]
# Checks out the pinned upstream DXMT commit (detached) in third_party/dxmt and applies patches/dxmt/*.patch
# uncommitted. build also configures third_party/dxmt/build (first time) and runs ninja; the DLLs end up in
# third_party/dxmt/build/src/{d3d11,dxgi,d3d10}/. Run scripts/fetch-deps.sh first.
set -euo pipefail

ROOT=${0:A:h:h}
DXMT="$ROOT/third_party/dxmt"
BASE=50102f994b406f58f56d5d09f2ad20f9b90390a2
PATCHES=("$ROOT"/patches/dxmt/*.patch)

if [[ -n "$(git -C "$DXMT" status --porcelain)" ]]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    export GIT_INDEX_FILE="$tmp/index"
    git -C "$DXMT" read-tree "$BASE"
    for p in "${PATCHES[@]}"; do git -C "$DXMT" apply --cached "$p"; done
    series=$(git -C "$DXMT" write-tree)
    unset GIT_INDEX_FILE
    git -C "$DXMT" add -A
    current=$(git -C "$DXMT" write-tree)
    git -C "$DXMT" reset -q
    if [[ "$series" != "$current" ]]; then
        echo "third_party/dxmt has changes that are not this patch series:" >&2
        git -C "$DXMT" diff --stat "$series" "$current" >&2
        exit 1
    fi
    git -C "$DXMT" checkout -q -f --detach "$BASE"
    git -C "$DXMT" clean -fdq
else
    git -C "$DXMT" checkout -q --detach "$BASE"
fi

for p in "${PATCHES[@]}"; do
    git -C "$DXMT" apply "$p"
    echo "applied ${p:t}"
done

if [[ "${1:-}" == "build" ]]; then
    if [[ ! -d "$DXMT/build" ]]; then
        # wine_builtin_dll=false: built as native DLLs, otherwise Wine silently prefers CrossOver's own DXMT.
        meson setup --cross-file "$DXMT/build-win64.txt" --native-file "$ROOT/toolchains/native-x86_64.txt" \
            -Dbuildtype=release -Dnative_llvm_path="${MQL_LLVM_DIR:-$ROOT/toolchains/llvm}" \
            -Dwine_install_path="${MQL_WINE_DIR:-$ROOT/toolchains/wine}" \
            -Dwine_builtin_dll=false "$DXMT/build" "$DXMT"
    fi
    ninja -C "$DXMT/build"
fi
