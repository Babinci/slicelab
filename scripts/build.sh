#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
source scripts/env.sh
if [[ ! -x .toolchain/bin/cmake || ! -f .deps/.headers-ready ]]; then
    bash scripts/bootstrap-linux.sh
fi
cmake -S vendor/audio-plugin-coder -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG' \
    -DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG' \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DAPC_PLUGINS_DIR="$root/plugins" \
    -DAPC_ENABLE_VISAGE=OFF -DAPC_ENABLE_CLAP=OFF \
    -DCMAKE_PREFIX_PATH="$root/.deps/usr" \
    -DCMAKE_LIBRARY_PATH="$root/.deps/usr/lib/x86_64-linux-gnu" \
    -DCMAKE_INCLUDE_PATH="$root/.deps/usr/include"
targets=(SliceLab_VST3 SliceLab_Standalone)
if [[ $# -gt 0 ]]; then targets=("$@"); fi
cmake --build build --parallel "${SLICELAB_BUILD_JOBS:-2}" --target "${targets[@]}"
