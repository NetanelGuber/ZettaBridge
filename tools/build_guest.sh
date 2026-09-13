#!/bin/sh
# Builds arm32 guest test programs with the NDK into build/guest/.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-$HOME/android-ndk-r29}
NDK_HOST=${NDK_HOST:-linux-arm64}
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin/armv7a-linux-androideabi21-clang"
OUT="$ROOT/build/guest"

mkdir -p "$OUT"
"$CC" -c -o "$OUT/abi_check.o" "$ROOT/tools/abi_check.c"

for src in "$ROOT"/guest/tests/*_static.c; do
    name=$(basename "$src" .c)
    "$CC" -static -O2 -Wall -o "$OUT/$name" "$src"
done

echo "guest build ok"
