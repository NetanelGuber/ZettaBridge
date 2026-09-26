#!/usr/bin/env python3
"""Inventory generated Android platform calls and selected ARM32 ELF imports.

The classifications describe the Android runtime build. GLES/EGL handlers exist but their
correctness audit belongs to plan Step 12. A missing symbol in an ELF is unsupported at the
platform-stub boundary and is also reported by apk_preflight's linkage analysis.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
ROWS = ROOT / "core/src/gen/hostcalls.inc"
ROW = re.compile(r'^\{(\d+), "([^"]+)", "([^"]+)"\},$')
PLATFORM = re.compile(
    r"^(?:AAsset|ANativeWindow|ALooper|AInput|AKeyEvent|AMotionEvent|AConfiguration|"
    r"AndroidBitmap|ASensor|ASharedMemory|egl|gl[A-Z])"
)
EXPLICIT_FAILURE = {174, 175, 177, 178, 180, 214, 215, 216}
EXPLICIT_FAILURE_NAMES = {"AInputEvent_release", "AInputEvent_toJava",
                          "AKeyEvent_fromJava", "AMotionEvent_fromJava"}


def inventory():
    result = []
    for line in ROWS.read_text(encoding="utf-8").splitlines()[1:]:
        match = ROW.fullmatch(line)
        if not match:
            raise ValueError(f"malformed host call: {line}")
        index, library, name = int(match[1]), match[2], match[3]
        if index != len(result):
            raise ValueError(f"host-call index gap or duplicate at {index}")
        status = ("explicit failure" if index in EXPLICIT_FAILURE or name in EXPLICIT_FAILURE_NAMES else
                  "pending" if library in ("libEGL.so", "libGLESv2.so") else "implemented")
        result.append({"index": index, "library": library, "name": name, "status": status})
    if len(result) != 459:
        raise ValueError(f"expected 459 calls after Step 11, found {len(result)}")
    for entry in result:
        name = entry["name"]
        index = entry["index"]
        if 142 <= index <= 159 and not name.startswith("AAsset"):
            raise ValueError("asset indices changed")
        if 344 <= index <= 394 and not name.startswith("AConfiguration_"):
            raise ValueError("configuration indices changed")
        if 395 <= index <= 401 and not name.startswith("AInputQueue_"):
            raise ValueError("input-queue indices changed")
        if 402 <= index <= 458 and not name.startswith(("AInputEvent_", "AKeyEvent_", "AMotionEvent_")):
            raise ValueError("input-event indices changed")
    return result


def elf_imports(path):
    output = subprocess.check_output(["readelf", "--wide", "--dyn-syms", str(path)], text=True)
    names = set()
    for line in output.splitlines():
        columns = line.split()
        if len(columns) >= 8 and columns[6] == "UND":
            names.add(columns[7].split("@", 1)[0])
    return sorted(name for name in names if PLATFORM.match(name))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check the indexed inventory")
    parser.add_argument("--json", action="store_true", help="print every generated call")
    parser.add_argument("--elf", type=Path, action="append", default=[], help="ARM32 ELF to inspect")
    args = parser.parse_args()
    rows = inventory()
    by_name = {entry["name"]: entry for entry in rows}
    imports = {}
    for path in args.elf:
        imports[str(path)] = [
            {"name": name, "status": by_name[name]["status"] if name in by_name else "unsupported"}
            for name in elf_imports(path)
        ]
    if args.json:
        print(json.dumps({"host_calls": rows, "imports": imports}, indent=2))
    else:
        counts = {status: sum(row["status"] == status for row in rows)
                  for status in ("implemented", "explicit failure", "unsupported", "pending")}
        print(f"generated calls: {len(rows)}; " + ", ".join(f"{key}: {value}" for key, value in counts.items()))
        for path, entries in imports.items():
            print(f"{path}: {len(entries)} platform imports")
            for entry in entries:
                print(f"  {entry['status']}: {entry['name']}")


if __name__ == "__main__":
    main()
