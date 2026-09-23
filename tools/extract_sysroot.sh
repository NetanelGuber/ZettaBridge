#!/bin/sh
# Extract the arm32 bionic sysroot from the pinned Android 17 QPR2 AOSP GSI.
# Source and terms: https://developer.android.com/about/versions/17/qpr2/gsi-release-notes
# The GSI is for app validation. Do not redistribute the archive or derived sysroot
# except as allowed by the terms included with that specific download.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
URL=https://dl.google.com/developers/android/cinnamonbun/images/gsi/aosp_arm64-exp-CP41.260814.003.B1-16166531-e6cb3bc5.zip
SHA=e6cb3bc521fb4a8b4c8e62f8557c6ae0ff10662a6838cfb346a32ec9c9134e22
ARCHIVE_BYTES=1173930919
MAX_IMAGE_BYTES=17179869184
SAFETY_BYTES=67108864
CACHE="$ROOT/.cache/gsi"
OUT="$ROOT/sysroot"
ZIP="$CACHE/gsi.zip"
ZIP_PART="$CACHE/gsi.zip.part"
IMG="$CACHE/system.img"
IMG_PART="$CACHE/system.img.part"
IMG_SHA="$CACHE/system.img.sha256"

for command in curl debugfs df sha256sum unzip awk; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "required command not found: $command" >&2
        exit 1
    fi
done

mkdir -p "$CACHE" "$OUT/system/lib" "$OUT/system/bin"

size_bytes() {
    wc -c < "$1" | tr -d '[:space:]'
}

available_bytes() {
    available_kb=$(df -Pk "$1" | awk 'END { print $4 }')
    case "$available_kb" in
        ''|*[!0-9]*) echo "cannot determine free space for $1" >&2; exit 1 ;;
    esac
    echo $((available_kb * 1024))
}

require_space() {
    path=$1
    needed=$2
    available=$(available_bytes "$path")
    if [ "$available" -lt "$needed" ]; then
        echo "insufficient free space at $path: need $needed bytes, have $available" >&2
        exit 1
    fi
}

if [ ! -f "$ZIP" ]; then
    partial_bytes=0
    if [ -f "$ZIP_PART" ]; then
        partial_bytes=$(size_bytes "$ZIP_PART")
    fi
    if [ "$partial_bytes" -gt "$ARCHIVE_BYTES" ]; then
        echo "partial GSI archive is larger than expected; remove $ZIP_PART and retry" >&2
        exit 1
    fi
    remaining_bytes=$((ARCHIVE_BYTES - partial_bytes))
    require_space "$CACHE" $((remaining_bytes + SAFETY_BYTES))
    echo "downloading/resuming Android 17 GSI archive ($ARCHIVE_BYTES bytes)"
    curl --fail --location --retry 3 --continue-at - \
        --max-filesize "$ARCHIVE_BYTES" --output "$ZIP_PART" "$URL"
    actual_bytes=$(size_bytes "$ZIP_PART")
    if [ "$actual_bytes" -ne "$ARCHIVE_BYTES" ]; then
        echo "GSI archive size mismatch: expected $ARCHIVE_BYTES, found $actual_bytes" >&2
        exit 1
    fi
    echo "$SHA  $ZIP_PART" | sha256sum --check --status || {
        echo "GSI archive SHA-256 mismatch; remove $ZIP_PART and retry" >&2
        exit 1
    }
    mv "$ZIP_PART" "$ZIP"
elif [ "$(size_bytes "$ZIP")" -ne "$ARCHIVE_BYTES" ]; then
    echo "cached GSI archive size mismatch; inspect or remove $ZIP and retry" >&2
    exit 1
fi

echo "$SHA  $ZIP" | sha256sum --check --status || {
    echo "cached GSI archive SHA-256 mismatch; inspect or remove $ZIP and retry" >&2
    exit 1
}
unzip -tq "$ZIP"

IMAGE_BYTES=$(unzip -l "$ZIP" | awk '
    $NF == "system.img" && $1 ~ /^[0-9]+$/ { count++; size = $1 }
    END { if (count != 1 || size <= 0) exit 1; print size }
') || {
    echo "expected exactly one non-empty system.img in the verified GSI archive" >&2
    exit 1
}
if [ "$IMAGE_BYTES" -gt "$MAX_IMAGE_BYTES" ]; then
    echo "system.img exceeds the $MAX_IMAGE_BYTES byte extraction limit" >&2
    exit 1
fi

image_valid=0
if [ -f "$IMG" ] && [ -f "$IMG_SHA" ] && [ "$(size_bytes "$IMG")" -eq "$IMAGE_BYTES" ]; then
    recorded_sha=$(cat "$IMG_SHA")
    actual_sha=$(sha256sum "$IMG" | awk '{ print $1 }')
    if [ "$recorded_sha" = "$actual_sha" ]; then
        image_valid=1
    fi
elif [ -f "$IMG" ] && [ "$(size_bytes "$IMG")" -eq "$IMAGE_BYTES" ]; then
    # Adopt an image left by the previous extractor only if it exactly matches the
    # verified archive entry. This also covers upgrades from the older script.
    archive_sha=$(unzip -p "$ZIP" system.img | sha256sum | awk '{ print $1 }')
    actual_sha=$(sha256sum "$IMG" | awk '{ print $1 }')
    if [ "$archive_sha" = "$actual_sha" ]; then
        printf '%s\n' "$archive_sha" > "$IMG_SHA"
        image_valid=1
    fi
fi

if [ "$image_valid" -ne 1 ]; then
    require_space "$CACHE" $((IMAGE_BYTES + SAFETY_BYTES))
    # A partial image is not trusted. The verified archive is retained, so an
    # interrupted extraction can restart safely without downloading it again.
    rm -f "$IMG_PART"
    echo "extracting verified system.img ($IMAGE_BYTES bytes)"
    unzip -p "$ZIP" system.img > "$IMG_PART"
    actual_bytes=$(size_bytes "$IMG_PART")
    if [ "$actual_bytes" -ne "$IMAGE_BYTES" ]; then
        echo "system.img size mismatch: expected $IMAGE_BYTES, found $actual_bytes" >&2
        exit 1
    fi
    actual_sha=$(sha256sum "$IMG_PART" | awk '{ print $1 }')
    mv "$IMG_PART" "$IMG"
    printf '%s\n' "$actual_sha" > "$IMG_SHA.part"
    mv "$IMG_SHA.part" "$IMG_SHA"
else
    echo "reusing verified system.img"
fi

dump() {
    guest_path=$1
    destination=$2
    digest_file="$destination.sha256"

    if [ -s "$destination" ] && [ -f "$digest_file" ]; then
        recorded_sha=$(cat "$digest_file")
        actual_sha=$(sha256sum "$destination" | awk '{ print $1 }')
        if [ "$recorded_sha" = "$actual_sha" ]; then
            echo "reusing $guest_path"
            return
        fi
    fi

    partial="$destination.part"
    digest_partial="$digest_file.part"
    rm -f "$partial" "$digest_partial"
    if ! debugfs -R "dump \"$guest_path\" \"$partial\"" "$IMG" >/dev/null 2>&1; then
        echo "failed to extract $guest_path from verified system.img" >&2
        exit 1
    fi
    if [ ! -s "$partial" ]; then
        echo "missing or empty file in GSI: $guest_path" >&2
        exit 1
    fi

    actual_sha=$(sha256sum "$partial" | awk '{ print $1 }')
    mv "$partial" "$destination"
    printf '%s\n' "$actual_sha" > "$digest_partial"
    mv "$digest_partial" "$digest_file"
    echo "extracted $guest_path"
}

dump /system/bin/bootstrap/linker "$OUT/system/bin/linker"
for file in libc.so libm.so libdl.so libdl_android.so; do
    dump "/system/lib/bootstrap/$file" "$OUT/system/lib/$file"
done
for file in ld-android.so liblog.so libz.so libc++.so libstdc++.so; do
    dump "/system/lib/$file" "$OUT/system/lib/$file"
done

echo "verified arm32 sysroot ready in $OUT"
echo "GSI archive and system image are cached in $CACHE; remove them manually to reclaim space"
