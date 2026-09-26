#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-$HOME/android-ndk-r29}
NDK_HOST=${NDK_HOST:-linux-arm64}
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin/armv7a-linux-androideabi21-clang"
OUT="$ROOT/build/step11fixture/jniLibs/armeabi-v7a"
mkdir -p "$OUT"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wl,-soname,libzbstep11probe.so \
    -o "$OUT/libzbstep11probe.so" "$ROOT/guest/testlib/zbstep11probe.c" \
    -L"$ROOT/build/guest/lib" -landroid -ljnigraphics
