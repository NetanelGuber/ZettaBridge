#!/bin/sh
# Initialize the pinned Dynarmic checkout and apply the fork's required local patches.
# Safe to rerun: an applied patch is detected by its reverse applicability check.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SUBMODULE_REL=third_party/dynarmic
SUBMODULE="$ROOT/$SUBMODULE_REL"

git -C "$ROOT" submodule update --init -- "$SUBMODULE_REL"

EXPECTED=$(git -C "$ROOT" rev-parse "HEAD:$SUBMODULE_REL")
ACTUAL=$(git -C "$SUBMODULE" rev-parse HEAD)
if [ "$ACTUAL" != "$EXPECTED" ]; then
    echo "Dynarmic revision mismatch: expected $EXPECTED, found $ACTUAL" >&2
    exit 1
fi

for name in \
    dynarmic-0001-thumb32-armv8.patch \
    dynarmic-0002-asimd-narrowing.patch
do
    patch="$ROOT/third_party/patches/$name"
    if git -C "$SUBMODULE" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$SUBMODULE" apply "$patch"
        echo "applied $name"
    elif git -C "$SUBMODULE" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "already applied $name"
    else
        echo "cannot apply $name to pinned Dynarmic $EXPECTED; inspect the submodule worktree" >&2
        exit 1
    fi
done

echo "Dynarmic ready at $ACTUAL; submodule revision unchanged"
