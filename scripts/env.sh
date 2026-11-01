#!/usr/bin/env bash
slicelab_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$slicelab_root/.toolchain/bin:$PATH"
export CMAKE_BUILD_PARALLEL_LEVEL="${SLICELAB_BUILD_JOBS:-2}"
export PKG_CONFIG_PATH="$slicelab_root/.deps/usr/lib/x86_64-linux-gnu/pkgconfig:$slicelab_root/.deps/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_SYSROOT_DIR="$slicelab_root/.deps"
export CPATH="$slicelab_root/.deps/usr/include:$slicelab_root/.deps/usr/include/x86_64-linux-gnu${CPATH:+:$CPATH}"
export LIBRARY_PATH="$slicelab_root/.deps/usr/lib/x86_64-linux-gnu${LIBRARY_PATH:+:$LIBRARY_PATH}"
