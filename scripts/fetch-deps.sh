#!/bin/zsh
# Usage: scripts/fetch-deps.sh [sources|toolchains|all]   (default: all)
# sources:    third_party/dxmt (DXMT at the pinned commit, with submodules), third_party/nv-codec-headers,
#             third_party/openvr (headers only are used)
# toolchains: toolchains/wine (3Shain's Wine 8.16 build, DXMT's import libraries) and toolchains/llvm (LLVM 15.0.7 for
#             x86_64 macOS, built like DXMT's CI; takes a while), plus toolchains/native-x86_64.txt for meson
# MQL_LLVM_DIR / MQL_WINE_DIR use (or create) the LLVM install / Wine tree elsewhere; scripts/dxmt-patch.sh reads them too.
set -euo pipefail

ROOT=${0:A:h:h}
DXMT_URL=https://github.com/3Shain/dxmt.git
DXMT_COMMIT=50102f994b406f58f56d5d09f2ad20f9b90390a2
NVCODEC_URL=https://github.com/FFmpeg/nv-codec-headers.git
NVCODEC_COMMIT=eddcea9e27f6b772057c9b3f87de2cc1737faffc
OPENVR_URL=https://github.com/ValveSoftware/openvr.git
OPENVR_COMMIT=0924064316de3effbcd1acf1e309182a2deb1c05
WINE_URL=https://github.com/3Shain/wine/releases/download/v8.16-3shain/wine.tar.gz
LLVM_TAG=llvmorg-15.0.7

fetch() {
    local dir=$ROOT/third_party/$1 url=$2 commit=$3
    if [[ ! -d $dir/.git ]]; then
        git clone --filter=blob:none "$url" "$dir"
    fi
    git -C "$dir" fetch -q origin
    git -C "$dir" checkout -q --detach "$commit"
    echo "$1 at $commit"
}

sources() {
    mkdir -p "$ROOT/third_party"
    fetch dxmt $DXMT_URL $DXMT_COMMIT
    git -C "$ROOT/third_party/dxmt" submodule update --init --recursive
    fetch nv-codec-headers $NVCODEC_URL $NVCODEC_COMMIT
    fetch openvr $OPENVR_URL $OPENVR_COMMIT
}

toolchains() {
    local tc=$ROOT/toolchains
    local wine=${MQL_WINE_DIR:-$tc/wine} llvm=${MQL_LLVM_DIR:-$tc/llvm}
    mkdir -p "$tc"
    if [[ ! -d $wine/lib ]]; then
        mkdir -p "$wine"
        curl -fSL --retry 3 "$WINE_URL" -o "$tc/wine.tar.gz"
        tar -xzf "$tc/wine.tar.gz" -C "$wine"
        rm "$tc/wine.tar.gz"
    fi
    if [[ ! -d $llvm/lib ]]; then
        [[ -d $tc/llvm-project ]] || git clone --depth 1 --branch $LLVM_TAG https://github.com/llvm/llvm-project.git "$tc/llvm-project"
        cmake -B "$tc/llvm-build" -S "$tc/llvm-project/llvm" -G Ninja \
            -DCMAKE_INSTALL_PREFIX="$llvm" \
            -DCMAKE_OSX_ARCHITECTURES=x86_64 \
            -DLLVM_HOST_TRIPLE=x86_64-apple-darwin \
            -DLLVM_ENABLE_ASSERTIONS=On \
            -DLLVM_ENABLE_ZSTD=Off \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_FLAGS="-D_LIBCPP_KEEP_TRANSITIVE_INCLUDES_LLVM23" \
            -DLLVM_TARGETS_TO_BUILD="" \
            -DLLVM_BUILD_TOOLS=Off \
            -DLLVM_INCLUDE_BENCHMARKS=Off
        cmake --build "$tc/llvm-build"
        cmake --install "$tc/llvm-build"
    fi
    cat > "$tc/native-x86_64.txt" <<'EOF'
[binaries]
c = ['clang', '-arch', 'x86_64']
cpp = ['clang++', '-arch', 'x86_64']
objc = ['clang', '-arch', 'x86_64']
objcpp = ['clang++', '-arch', 'x86_64']

[host_machine]
system = 'darwin'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
    echo "toolchains ready: llvm $llvm, wine $wine"
}

case "${1:-all}" in
sources) sources ;;
toolchains) toolchains ;;
all) sources; toolchains ;;
*) echo "usage: $0 [sources|toolchains|all]" >&2; exit 1 ;;
esac
