#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-$HOME/android-ndk-r29}
NDK_HOST=${NDK_HOST:-linux-arm64}
CXX="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin/aarch64-linux-android33-clang++"
OUT="$ROOT/build/step11inputfixture/jniLibs/arm64-v8a"
mkdir -p "$OUT"
"$CXX" -shared -fPIC -O2 -Wall -Wextra -std=c++20 -static-libstdc++ \
    -I"$ROOT/core/include" \
    -Wl,-soname,libzbinputtest.so -o "$OUT/libzbinputtest.so" \
    "$ROOT/core/android/input_driver_backend.cpp" \
    "$ROOT/tests/device/step11_input_driver_probe.cpp" -landroid -llog
