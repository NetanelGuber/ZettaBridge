#!/bin/sh
# Assemble the standalone installed-app bootstrap probe. Never touches an installed app.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/step04"
BASE="$ROOT/build/launcher"
for file in "$BASE/assets/zb-files.txt" "$BASE/jniLibs/arm64-v8a/libzbridge.so" \
        "$BASE/assets/zb/host/libzbproxy.so" "$ROOT/build/guest/lib/libzbstep04a.so" \
        "$ROOT/build/guest/lib/libzbstep04b.so"; do
    if [ ! -f "$file" ]; then
        echo "missing $file; build guest, Android core and launcher bundle first" >&2
        exit 1
    fi
done
rm -rf "$OUT"
mkdir -p "$OUT/assets/zb/app/lib" "$OUT/jniLibs/arm64-v8a"
cp -a "$BASE/assets/zb/." "$OUT/assets/zb/"
cp "$ROOT/build/guest/lib/libzbstep04a.so" "$ROOT/build/guest/lib/libzbstep04b.so" \
    "$OUT/assets/zb/app/lib/"
cp "$BASE/jniLibs/arm64-v8a/libzbridge.so" "$OUT/jniLibs/arm64-v8a/"
for name in zbstep04a zbstep04b; do
    cp "$BASE/assets/zb/host/libzbproxy.so" "$OUT/jniLibs/arm64-v8a/lib$name.so"
done
(cd "$OUT/assets" && find zb -type f | LC_ALL=C sort) > "$OUT/assets/zb-files.txt"
VERSION=$(cd "$OUT/assets" && while IFS= read -r file; do sha256sum "$file"; done < zb-files.txt | sha256sum | cut -d' ' -f1)
printf '%s\n' "$VERSION" > "$OUT/assets/zb-version.txt"
python3 "$ROOT/tools/check_zbproxy.py" "$OUT/jniLibs/arm64-v8a/libzbstep04a.so"
python3 "$ROOT/tools/check_zbproxy.py" "$OUT/jniLibs/arm64-v8a/libzbstep04b.so"
echo "step04 probe bundle ready: $OUT"
