#!/bin/sh
# Runs arm32 guest test programs under zbrun and checks exit code and stdout.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZBRUN=${ZBRUN:-$ROOT/build/host/cli/zbrun/zbrun}
GUEST="$ROOT/build/guest"
EXPECTED="$ROOT/guest/tests/expected"
failures=0

run_case() {
    name=$1
    expected_exit=$2
    shift 2
    out=$("$ZBRUN" "$GUEST/$name" "$@" 2>"$GUEST/$name.stderr")
    code=$?
    if [ "$code" != "$expected_exit" ]; then
        echo "FAIL $name: exit $code, expected $expected_exit (stderr in build/guest/$name.stderr)"
        failures=$((failures + 1))
        return
    fi
    if ! printf '%s\n' "$out" | diff -u "$EXPECTED/$name.out" - >"$GUEST/$name.diff"; then
        echo "FAIL $name: stdout differs (see build/guest/$name.diff)"
        failures=$((failures + 1))
        return
    fi
    echo "PASS $name"
}

run_case hello_static 7 world

if [ "$failures" -ne 0 ]; then
    echo "$failures guest test(s) failed"
    exit 1
fi
echo "all guest tests passed"
