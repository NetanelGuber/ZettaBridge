#!/bin/sh
# Builds arm32 guest code with the NDK into build/guest/.
#   guest/tests/*_static.c   -> static executables (no guest linker needed)
#   guest/tests/*_dynamic.c  -> dynamic PIE executables (run with --sysroot)
#   guest/stubs/gen/*.S      -> build/guest/lib/<lib>.so host-call stub libraries
#   guest/compat/zbcompat.c  -> build/guest/lib/libzbcompat.so
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-$HOME/android-ndk-r29}
NDK_HOST=${NDK_HOST:-linux-arm64}
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin/armv7a-linux-androideabi21-clang"
OUT="$ROOT/build/guest"

mkdir -p "$OUT/lib"
"$CC" -c -o "$OUT/abi_check.o" "$ROOT/tools/abi_check.c"

for src in "$ROOT"/guest/tests/*_static.c; do
    name=$(basename "$src" .c)
    "$CC" -static -O2 -Wall -o "$OUT/$name" "$src"
done

for src in "$ROOT"/guest/tests/*_dynamic.c; do
    name=$(basename "$src" .c)
    "$CC" -O2 -Wall -o "$OUT/$name" "$src"
done

for asm in "$ROOT"/guest/stubs/gen/*.S; do
    lib=$(basename "$asm" .S)
    "$CC" -shared -nostdlib -Wl,-soname,"$lib.so" -o "$OUT/lib/$lib.so" "$asm"
done

"$CC" -shared -O2 -Wall -Wl,-soname,libzbcompat.so -o "$OUT/lib/libzbcompat.so" "$ROOT/guest/compat/zbcompat.c"

echo "guest build ok"
