#!/bin/sh
# Builds arm32 guest code with the NDK into build/guest/.
#   guest/tests/*_static.c      -> static executables (no guest linker needed)
#   guest/tests/*_dynamic.c     -> dynamic PIE executables (run with --sysroot)
#   guest/tests/*_dynamic.cpp   -> dynamic C++ executables linked with libzbthrow + libc++_shared
#   guest/testlib/zbthrow.cpp   -> build/guest/lib/libzbthrow.so
#   guest/testlib/zbcallprobe.c -> build/guest/lib/libzbcallprobe.so
#   guest/zbhost/zbhost.c       -> build/guest/zbhost
#   guest/stubs/gen/*.S         -> build/guest/lib/<lib>.so host-call stub libraries
#   guest/compat/zbcompat.c     -> build/guest/lib/libzbcompat.so
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-$HOME/android-ndk-r29}
NDK_HOST=${NDK_HOST:-linux-arm64}
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$NDK_HOST"
CC="$TOOLCHAIN/bin/armv7a-linux-androideabi21-clang"
CXX="$TOOLCHAIN/bin/armv7a-linux-androideabi21-clang++"
OUT="$ROOT/build/guest"

mkdir -p "$OUT/lib"
"$CC" -c -o "$OUT/abi_check.o" "$ROOT/tools/abi_check.c"

for src in "$ROOT"/guest/tests/*_static.c; do
    name=$(basename "$src" .c)
    "$CC" -static -O2 -Wall -o "$OUT/$name" "$src"
done

for src in "$ROOT"/guest/tests/*_dynamic.c; do
    name=$(basename "$src" .c)
    "$CC" -O2 -Wall -o "$OUT/$name" "$src" -llog
done

"$CC" -O2 -Wall -I"$ROOT/core/include" -o "$OUT/zbhost" \
    "$ROOT/guest/zbhost/zbhost.c" -ldl

cp "$TOOLCHAIN/sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so" "$OUT/lib/"
"$CXX" -shared -O2 -Wall -nostdlib++ -Wl,-soname,libzbthrow.so -o "$OUT/lib/libzbthrow.so" \
    "$ROOT/guest/testlib/zbthrow.cpp" -lc++_shared
# Base AAPCS probe library for library_runtime_test.
"$CC" -shared -fPIC -O2 -Wall -Wl,-soname,libzbcallprobe.so -o "$OUT/lib/libzbcallprobe.so" \
    "$ROOT/guest/testlib/zbcallprobe.c"
for src in "$ROOT"/guest/tests/*_dynamic.cpp; do
    name=$(basename "$src" .cpp)
    "$CXX" -O2 -Wall -nostdlib++ -o "$OUT/$name" "$src" -L"$OUT/lib" -lzbthrow -lc++_shared
done

for asm in "$ROOT"/guest/stubs/gen/*.S; do
    lib=$(basename "$asm" .S)
    "$CC" -shared -nostdlib -Wl,-soname,"$lib.so" -o "$OUT/lib/$lib.so" "$asm"
done

"$CC" -shared -O2 -Wall -Wl,-soname,libzbcompat.so -o "$OUT/lib/libzbcompat.so" "$ROOT/guest/compat/zbcompat.c"

echo "guest build ok"
