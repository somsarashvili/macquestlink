#!/bin/zsh
# Usage: scripts/package.sh [version]   (default: git describe, or "dev")
# Packs a release after fetch-deps.sh, dxmt-patch.sh build, build.sh and build.sh tools:
# dist/macquestlink-<version>.tar.gz (+ .sha256), laid out like the repo with the binaries in build/
# (build/dxmt/ holds the patched d3d11/dxgi/d3d10core), so the scripts and build.sh install work from it.
set -euo pipefail

ROOT=${0:A:h:h}
VERSION=${1:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}
NAME=macquestlink-$VERSION
DIST=$ROOT/dist
TAR=$DIST/$NAME.tar
DXMT_DLLS=(d3d11/d3d11.dll dxgi/dxgi.dll d3d10/d3d10core.dll)
FILES=(README.md LICENSE THIRD_PARTY.md ROADMAP.md config docs scripts patches/dxmt tools/*.py
    build/nvEncodeAPI64.dll build/wtsapi32.dll build/macquestlink-encoder build/tools)

cd "$ROOT"
for f in $FILES third_party/dxmt/build/src/$^DXMT_DLLS third_party/openvr/LICENSE; do
    [[ -e $f ]] || { echo "missing $f (build first)" >&2; exit 1; }
done

mkdir -p "$DIST"
TAROPTS=(--uid 0 --gid 0 --uname root --gname root --no-mac-metadata --exclude .DS_Store --exclude __pycache__)
COPYFILE_DISABLE=1 tar -cf "$TAR" $TAROPTS -s ",^,$NAME/," $FILES
COPYFILE_DISABLE=1 tar -rf "$TAR" $TAROPTS -s ",^.*/,$NAME/build/dxmt/," -C third_party/dxmt/build/src $DXMT_DLLS
COPYFILE_DISABLE=1 tar -rf "$TAR" $TAROPTS -s ",^LICENSE\$,$NAME/build/tools/openvr_api.LICENSE," -C third_party/openvr LICENSE
gzip -9f "$TAR"
(cd "$DIST" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
echo "$TAR.gz"
