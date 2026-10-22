#!/bin/zsh
# Usage: [BOTTLE=name|path] scripts/build.sh [install|tools]
# Builds the NVENC shim (nvEncodeAPI64.dll), the wtsapi32 shim and the VideoToolbox helper into build/.
# install also copies them into the bottle (shim and helper are a pair: install both, never one alone).
# tools builds the diagnostic tools into build/tools.
# In an unpacked release (no sources), install only copies the prebuilt files from build/.
set -euo pipefail

ROOT=${0:A:h:h}
[[ -f $ROOT/local.env ]] && source $ROOT/local.env
BOTTLE=${BOTTLE:-SteamX64}
[[ $BOTTLE == /* ]] || BOTTLE=${BOTTLE_PATH:-$HOME/CXPBottles}/$BOTTLE
STEAM="$BOTTLE/drive_c/Program Files (x86)/Steam"
OUT="$ROOT/build"
WIN_CC=(x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-parameter)

mkdir -p "$OUT"

if [[ "${1:-}" == "tools" ]]; then
    mkdir -p "$OUT/tools"
    OPENVR=(-std=gnu17 -Wno-unused-variable -I"$ROOT/third_party/openvr/headers")
    for t in d3dshare kmstress d3dprobe sharefill angletex tsquery audioprobe threadtop; do
        "${WIN_CC[@]}" -o "$OUT/tools/$t.exe" "$ROOT/tools/$t.c" -ld3d11 -ldxgi -ld3dcompiler -lole32 -luuid -ldxguid -lpsapi
    done
    for t in vrcolors vroverlay vrstats vrsetting frametiming vrgputime; do
        "${WIN_CC[@]}" "${OPENVR[@]}" -o "$OUT/tools/$t.exe" "$ROOT/tools/$t.c" -ld3d11 -ldxgi -ld3dcompiler -lole32 -luuid -ldxguid -lgdi32
    done
    "${WIN_CC[@]}" -o "$OUT/tools/client_test.exe" "$ROOT/tools/client_test.c" "$ROOT/nvenc_shim/encoder_client.c" \
        -ld3d11 -lws2_32 -ldxguid -luuid
    clang -O2 -Wall -o "$OUT/tools/mkaudiodev" "$ROOT/tools/mkaudiodev.c" -framework CoreAudio -framework CoreFoundation
    clang -O2 -Wall -o "$OUT/tools/macthreads" "$ROOT/tools/macthreads.c"
    clang -O2 -Wall -o "$OUT/tools/encoder_test" "$ROOT/tools/encoder_test.c"
    clang -O2 -Wall -o "$OUT/tools/vtbench" "$ROOT/tools/vtbench.c" \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation
    cp "$ROOT/third_party/openvr/bin/win64/openvr_api.dll" "$OUT/tools/"
    echo "tools -> $OUT/tools"
    exit 0
fi

if [[ -d $ROOT/nvenc_shim ]]; then
    "${WIN_CC[@]}" -shared -I"$ROOT/third_party/nv-codec-headers/include" -o "$OUT/nvEncodeAPI64.dll" \
        "$ROOT"/nvenc_shim/{nvenc_shim,encoder_client,audio_hook,net_hook}.c -static-libgcc -Wl,--kill-at \
        -lws2_32 -ldxguid -lole32 -luuid

    "${WIN_CC[@]}" -shared -o "$OUT/wtsapi32.dll" "$ROOT/wts_shim/wtsapi32.c" "$ROOT/wts_shim/wtsapi32.def" \
        -static-libgcc -Wl,--kill-at

    clang -O2 -Wall -o "$OUT/macquestlink-encoder" "$ROOT/encoder/macquestlink_encoder.c" \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation -framework IOSurface
fi

# Copy to .new and rename: a running process keeps its mapped (old) file, nothing is overwritten in place.
install_file() {
    cp "$1" "$2.new" && mv "$2.new" "$2"
    echo "installed -> $2"
}

if [[ "${1:-}" == "install" ]]; then
    for dir in "$STEAM" "$STEAM/steamapps/common/SteamVR/bin/win64"; do
        install_file "$OUT/nvEncodeAPI64.dll" "$dir/nvEncodeAPI64.dll"
    done
    install_file "$OUT/wtsapi32.dll" "$STEAM/steamapps/common/SteamVR/bin/win64/wtsapi32.dll"
    mkdir -p "$BOTTLE/drive_c/macquestlink"
    install_file "$OUT/macquestlink-encoder" "$BOTTLE/drive_c/macquestlink/macquestlink-encoder"
fi
