#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SOURCE="$ROOT/build/guest/lib/libzbstep04a.so"
TARGET="$ROOT/build/step09fixture/jniLibs/armeabi-v7a"
if [ ! -f "$SOURCE" ]; then
    echo "missing $SOURCE; run tools/build_guest.sh first" >&2
    exit 1
fi
mkdir -p "$TARGET"
cp "$SOURCE" "$TARGET/"
