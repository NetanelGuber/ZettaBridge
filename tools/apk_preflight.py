#!/usr/bin/env python3
"""Read-only, bounded APK/split preflight. Requires Android SDK apksigner and readelf.

The JSON report is deliberately an inventory, not a promise of runtime compatibility.
No archive member is extracted to a caller-controlled path or executed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import xml.etree.ElementTree as ET
import zipfile

MAX_ARCHIVE = 1024 * 1024 * 1024
MAX_ENTRY = 128 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024
MAX_ENTRIES = 10000
MAX_MANIFEST = 4 * 1024 * 1024
MAX_LIB = 64 * 1024 * 1024
MAX_READELF_OUTPUT = 32 * 1024 * 1024
ANDROID = "{http://schemas.android.com/apk/res/android}"


class Invalid(Exception):
    pass


def check_name(name):
    parts = name.split("/")
    if (not name or name.startswith("/") or "\\" in name or "\x00" in name
            or any(p in ("", ".", "..") for p in parts if p != "")):
        raise Invalid(f"unsafe ZIP path: {name!r}")
    if any(not p for p in parts[:-1]):
        raise Invalid(f"unsafe ZIP path: {name!r}")
    if re.match(r"^[A-Za-z]:", name):
        raise Invalid(f"unsafe ZIP path: {name!r}")


def guarded_zip(path, max_total=MAX_TOTAL):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_ARCHIVE:
        raise Invalid(f"missing, symlinked or oversized archive: {path}")
    z = None
    try:
        z = zipfile.ZipFile(path)
        infos = z.infolist()
        if len(infos) > MAX_ENTRIES:
            raise Invalid("too many ZIP entries")
        names = set()
        total = 0
        for info in infos:
            name = info.filename
            check_name(name.rstrip("/") if info.is_dir() else name)
            if name in names:
                raise Invalid(f"duplicate ZIP entry: {name}")
            names.add(name)
            mode = (info.external_attr >> 16) & 0o170000
            if mode == 0o120000:
                raise Invalid(f"symlink ZIP entry: {name}")
            if mode not in (0, 0o040000, 0o100000):
                raise Invalid(f"unsupported ZIP entry type: {name}")
            if info.flag_bits & 1 or info.compress_type not in (0, 8):
                raise Invalid(f"encrypted or unsupported compression: {name}")
            if info.file_size > MAX_ENTRY:
                raise Invalid(f"oversized ZIP entry: {name}")
            total += info.file_size
            if total > max_total:
                raise Invalid("oversized expanded archive")
            if info.file_size > 1024 * max(info.compress_size, 1) and info.file_size > 1024 * 1024:
                raise Invalid(f"extreme compression ratio: {name}")
            if not info.is_dir():
                # Read all members to check CRC and enforce a real streamed byte limit.
                count = 0
                with z.open(info) as stream:
                    while chunk := stream.read(65536):
                        count += len(chunk)
                        if count > info.file_size or count > MAX_ENTRY:
                            raise Invalid(f"expanded size mismatch: {name}")
                if count != info.file_size:
                    raise Invalid(f"expanded size mismatch: {name}")
        return z, {i.filename: i for i in infos}
    except (zipfile.BadZipFile, EOFError, OSError, RuntimeError) as e:
        if z is not None:
            z.close()
        raise Invalid(f"invalid ZIP: {e}") from e
    except Exception:
        if z is not None:
            z.close()
        raise


def read_small(z, name, limit):
    info = z.getinfo(name)
    if info.file_size > limit:
        raise Invalid(f"oversized {name}")
    return z.read(name)


def u16(data, pos):
    if pos + 2 > len(data):
        raise Invalid("truncated binary manifest")
    return struct.unpack_from("<H", data, pos)[0]


def u32(data, pos):
    if pos + 4 > len(data):
        raise Invalid("truncated binary manifest")
    return struct.unpack_from("<I", data, pos)[0]


def varlen8(data, pos):
    first = data[pos]
    return ((first & 127) << 8) | data[pos + 1] if first & 128 else first


def string_pool(data, start, header, end):
    count, flags, strings_start = u32(data, start + 8), u32(data, start + 16), u32(data, start + 20)
    if count > 200000 or header < 28 or start + strings_start > end or start + header + count * 4 > end:
        raise Invalid("invalid manifest string pool")
    utf8 = bool(flags & 256)
    result = []
    for i in range(count):
        pos = start + strings_start + u32(data, start + header + i * 4)
        if pos >= end:
            raise Invalid("manifest string outside pool")
        if utf8:
            length = varlen8(data, pos)
            pos += 2 if data[pos] & 128 else 1
            length = varlen8(data, pos)
            pos += 2 if data[pos] & 128 else 1
            raw = data[pos:pos + length]
            if len(raw) != length or pos + length >= end or data[pos + length] != 0:
                raise Invalid("truncated UTF-8 manifest string")
            result.append(raw.decode("utf-8", "strict"))
        else:
            length = u16(data, pos)
            pos += 2
            if length & 0x8000:
                length = ((length & 0x7fff) << 16) | u16(data, pos)
                pos += 2
            raw = data[pos:pos + length * 2]
            if len(raw) != length * 2 or pos + length * 2 + 2 > end or u16(data, pos + length * 2):
                raise Invalid("truncated UTF-16 manifest string")
            result.append(raw.decode("utf-16le", "strict"))
    return result


def binary_manifest(data):
    if len(data) < 8 or u16(data, 0) != 3 or u16(data, 2) != 8 or u32(data, 4) != len(data):
        raise Invalid("invalid binary manifest header")
    strings = None
    stack = []
    root = None
    pos = 8
    while pos < len(data):
        typ, header, size = u16(data, pos), u16(data, pos + 2), u32(data, pos + 4)
        end = pos + size
        if size < header or header < 8 or end > len(data):
            raise Invalid("invalid binary manifest chunk")
        if typ == 1:
            strings = string_pool(data, pos, header, end)
        elif typ == 0x102:
            if strings is None or header != 16 or size < 36:
                raise Invalid("invalid manifest element")
            def lookup(index):
                if index == 0xffffffff:
                    return ""
                if index >= len(strings):
                    raise Invalid("manifest string index out of range")
                return strings[index]
            tag = lookup(u32(data, pos + 20))
            el = ET.Element(tag)
            attr_start, attr_size, count = u16(data, pos + 24), u16(data, pos + 26), u16(data, pos + 28)
            base = pos + 16 + attr_start
            if attr_size < 20 or count > 4096 or base + count * attr_size > end:
                raise Invalid("invalid manifest attributes")
            for i in range(count):
                at = base + i * attr_size
                ns, name, raw = u32(data, at), u32(data, at + 4), u32(data, at + 8)
                value_type, value = data[at + 15], u32(data, at + 16)
                key = ("{" + lookup(ns) + "}" if ns != 0xffffffff else "") + lookup(name)
                if raw != 0xffffffff:
                    val = lookup(raw)
                elif value_type == 3:
                    val = lookup(value)
                elif value_type == 18:
                    val = "true" if value else "false"
                elif value_type in (16, 17):
                    val = str(value)
                else:
                    val = f"@0x{value:08x}"
                if key in el.attrib:
                    raise Invalid("duplicate manifest attribute")
                el.set(key, val)
            if stack:
                stack[-1].append(el)
            elif root is None:
                root = el
            else:
                raise Invalid("multiple manifest roots")
            stack.append(el)
        elif typ == 0x103:
            if not stack or strings is None or u32(data, pos + 20) >= len(strings) or stack[-1].tag != strings[u32(data, pos + 20)]:
                raise Invalid("unbalanced binary manifest")
            stack.pop()
        pos = end
    if root is None or stack:
        raise Invalid("incomplete binary manifest")
    return root


def parse_manifest(data):
    try:
        root = binary_manifest(data) if data[:2] == b"\x03\x00" else ET.fromstring(data)
    except (ET.ParseError, UnicodeError, IndexError, ValueError) as e:
        raise Invalid(f"malformed manifest: {e}") from e
    if root.tag != "manifest" or not root.get("package"):
        raise Invalid("manifest lacks package")
    return root


def attr(el, name):
    return el.get(ANDROID + name)


def describe_manifest(root):
    sdk = root.find("uses-sdk")
    app = root.find("application")
    components = []
    if app is not None:
        for el in app:
            if el.tag in ("activity", "activity-alias", "service", "receiver", "provider"):
                components.append({"type": el.tag, "name": attr(el, "name"), "permission": attr(el, "permission"),
                                   "process": attr(el, "process"), "authorities": attr(el, "authorities")})
    native_activity = False
    if app is not None:
        for el in app.findall("activity"):
            if attr(el, "name") == "android.app.NativeActivity" or any(
                    attr(meta, "name") == "android.app.lib_name" for meta in el.findall("meta-data")):
                native_activity = True
    return {"package": root.get("package"), "version_code": attr(root, "versionCode"),
            "version_name": attr(root, "versionName"), "split": root.get("split"),
            "config_for_split": root.get("configForSplit"), "is_feature_split": root.get("isFeatureSplit"),
            "uses_splits": [attr(x, "name") for x in root.findall("uses-split")],
            "min_sdk": attr(sdk, "minSdkVersion") if sdk is not None else None,
            "target_sdk": attr(sdk, "targetSdkVersion") if sdk is not None else None,
            "shared_user_id": attr(root, "sharedUserId"),
            "permissions_requested": [attr(x, "name") for x in root.findall("uses-permission")],
            "permissions_declared": [{"name": attr(x, "name"), "protection_level": attr(x, "protectionLevel")}
                                     for x in root.findall("permission")],
            "features": [{"name": attr(x, "name"), "gl_es_version": attr(x, "glEsVersion"),
                          "required": attr(x, "required")} for x in root.findall("uses-feature")],
            "components": components, "extract_native_libs": attr(app, "extractNativeLibs") if app is not None else None,
            "has_code": attr(app, "hasCode") if app is not None else None,
            "native_activity": native_activity}


def signer(path, apksigner):
    try:
        run = subprocess.run([apksigner, "verify", "--verbose", "--print-certs", str(path)],
                             capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise Invalid(f"apksigner unavailable: {e}") from e
    if run.returncode:
        raise Invalid(f"APK signature verification failed: {(run.stderr or run.stdout).strip()[:500]}")
    digests = re.findall(r"Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]+)", run.stdout)
    if not digests:
        raise Invalid("apksigner reported no signer certificate")
    schemes = re.findall(r"Verified using (v\d+ scheme[^:]*): (true|false)", run.stdout)
    return {"cert_sha256": sorted(set(x.lower() for x in digests)),
            "verified_schemes": [scheme for scheme, yes in schemes if yes == "true"]}


def elf_report(data, readelf):
    if (len(data) < 52 or data[:4] != b"\x7fELF" or data[5] != 1
            or data[6] != 1 or data[4] not in (1, 2)):
        raise Invalid("malformed native ELF header")
    cls = data[4]
    if cls == 1:
        ehsize, phoff, phentsize, phnum = struct.unpack_from("<H", data, 40)[0], u32(data, 28), u16(data, 42), u16(data, 44)
        shoff, shentsize, shnum = u32(data, 32), u16(data, 46), u16(data, 48)
        expected_header, expected_ph, expected_sh = 52, 32, 40
    else:
        if len(data) < 64:
            raise Invalid("truncated ELF64 header")
        ehsize, phentsize, phnum = u16(data, 52), u16(data, 54), u16(data, 56)
        phoff, shoff = struct.unpack_from("<Q", data, 32)[0], struct.unpack_from("<Q", data, 40)[0]
        shentsize, shnum = u16(data, 58), u16(data, 60)
        expected_header, expected_ph, expected_sh = 64, 56, 64
    if (u16(data, 16) != 3 or ehsize != expected_header or phentsize != expected_ph
            or not phnum or phoff + phentsize * phnum > len(data)
            or (shnum and (shentsize != expected_sh or shoff + shentsize * shnum > len(data)))):
        raise Invalid("malformed native ELF layout or type")
    machine = struct.unpack_from("<H", data, 18)[0]
    with tempfile.NamedTemporaryFile(suffix=".so") as f:
        f.write(data)
        f.flush()
        try:
            with tempfile.TemporaryFile() as err:
                command = [readelf, "--wide", "--file-header", "--program-headers",
                           "--section-headers", "--dynamic", "--dyn-syms", "-A", f.name]
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=err)
                timer = threading.Timer(30, process.kill)
                timer.start()
                try:
                    chunks = []
                    size = 0
                    while chunk := process.stdout.read(65536):
                        size += len(chunk)
                        if size > MAX_READELF_OUTPUT:
                            raise Invalid("readelf output exceeds limit")
                        chunks.append(chunk)
                    code = process.wait()
                except Exception:
                    process.kill()
                    process.wait()
                    raise
                finally:
                    timer.cancel()
                err.seek(0)
                stderr = err.read(1024).decode("utf-8", "replace")
        except (OSError, subprocess.TimeoutExpired) as e:
            raise Invalid(f"readelf unavailable: {e}") from e
    output = b"".join(chunks).decode("utf-8", "replace")
    if code or "Error:" in stderr:
        raise Invalid(f"malformed native ELF: {stderr.strip()[:300]}")
    needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", output)
    imports, exports = [], []
    for line in output.splitlines():
        m = re.match(r"\s*\d+:\s+[0-9a-fA-F]+\s+\d+\s+\w+\s+\w+\s+\w+\s+(UND|\w+)\s+(.+)", line)
        if m:
            name = m.group(2).split("@", 1)[0].strip()
            if name:
                (imports if m.group(1) == "UND" else exports).append(name)
    attrs = [x.strip() for x in output.splitlines() if "Tag_CPU_arch:" in x or "Tag_ABI_VFP_args:" in x]
    return {"class": 32 if data[4] == 1 else 64, "machine": machine, "attributes": attrs,
            "needed": needed, "imports": sorted(set(imports)), "exports": sorted(set(exports)),
            "jni_hints": sorted(x for x in set(exports) if x == "JNI_OnLoad" or x.startswith("Java_")),
            "native_entry_points": sorted(x for x in set(exports) if x == "ANativeActivity_onCreate")}


def apk(path, apksigner, readelf):
    z, infos = guarded_zip(path)
    try:
        if "AndroidManifest.xml" not in infos:
            raise Invalid("APK lacks AndroidManifest.xml")
        manifest = describe_manifest(parse_manifest(read_small(z, "AndroidManifest.xml", MAX_MANIFEST)))
        libs = []
        embedded = []
        for name, info in infos.items():
            if name.lower().endswith(".apk"):
                embedded.append(name)
            parts = name.split("/")
            if len(parts) == 3 and parts[0] == "lib" and parts[2].endswith(".so"):
                if info.file_size > MAX_LIB:
                    raise Invalid(f"oversized native library: {name}")
                lib = elf_report(z.read(name), readelf)
                expected = {"armeabi": (32, 40), "armeabi-v7a": (32, 40), "arm64-v8a": (64, 183),
                            "x86": (32, 3), "x86_64": (64, 62)}.get(parts[1])
                if expected != (lib["class"], lib["machine"]):
                    raise Invalid(f"ABI/ELF mismatch or unknown ABI: {name}")
                if info.compress_type == 0:
                    with path.open("rb") as file:
                        file.seek(info.header_offset)
                        header = file.read(30)
                    if len(header) != 30 or header[:4] != b"PK\x03\x04":
                        raise Invalid(f"invalid local ZIP header: {name}")
                    local_name, local_extra = struct.unpack_from("<HH", header, 26)
                    data_offset = info.header_offset + 30 + local_name + local_extra
                else:
                    data_offset = None
                libs.append({"path": name, "abi": parts[1], "compressed": info.compress_type != 0,
                             "data_offset": data_offset, "aligned_4096": data_offset is not None and data_offset % 4096 == 0,
                             **lib})
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        return {"file": str(path), "sha256": digest, "expanded_bytes": sum(i.file_size for i in infos.values()),
                "manifest": manifest, "signer": signer(path, apksigner), "abis": sorted(set(x["abi"] for x in libs)),
                "libraries": libs, "embedded_apks": embedded, "entry_count": len(infos)}
    finally:
        z.close()


def analyze(paths, apksigner, readelf):
    if len(paths) == 1 and paths[0].suffix.lower() == ".aab":
        raise Invalid("AAB input is outside Step 02 scope")
    with tempfile.TemporaryDirectory(prefix="zb-preflight-") as temp:
        container = paths[0] if len(paths) == 1 and paths[0].suffix.lower() == ".apks" else None
        if container is not None:
            z, infos = guarded_zip(paths[0])
            try:
                members = [n for n in infos if n.endswith(".apk")]
                if not members or len(members) > 128:
                    raise Invalid(".apks has no APKs or too many APKs")
                extracted = []
                for index, name in enumerate(members):
                    dest = Path(temp) / f"{index}.apk"
                    with z.open(name) as src, dest.open("wb") as out:
                        shutil.copyfileobj(src, out, 65536)
                    extracted.append(dest)
                paths = extracted
            finally:
                z.close()
        if not paths or any(p.suffix.lower() != ".apk" for p in paths):
            raise Invalid("input must be APK(s) or one .apks container")
        if len(paths) > 128:
            raise Invalid("too many APK splits")
        if sum(p.stat().st_size for p in paths) > MAX_ARCHIVE:
            raise Invalid("oversized APK set")
        result = [apk(p, apksigner, readelf) for p in paths]
        if sum(r["expanded_bytes"] for r in result) > MAX_TOTAL or sum(r["entry_count"] for r in result) > MAX_ENTRIES:
            raise Invalid("APK set exceeds expanded byte or entry limit")
        if container is not None:
            for item, member in zip(result, members):
                item["file"] = str(container) + "!" + member
        base = [r for r in result if not r["manifest"]["split"]]
        names = [r["manifest"]["split"] for r in result if r["manifest"]["split"]]
        if len(base) != 1 or len(names) != len(set(names)):
            raise Invalid("split set needs exactly one base and unique split names")
        first = base[0]
        identity = (first["manifest"]["package"], first["manifest"]["version_code"],
                    first["manifest"]["version_name"],
                    first["signer"]["cert_sha256"])
        for r in result:
            m = r["manifest"]
            if (m["package"], m["version_code"], m["version_name"], r["signer"]["cert_sha256"]) != identity:
                raise Invalid("mixed package, version or signer in split set")
            for dep in m["uses_splits"] + ([m["config_for_split"]] if m["config_for_split"] else []):
                if dep and dep not in names and dep != "base":
                    raise Invalid(f"missing required split: {dep}")
        findings = []
        def finding(level, category, kind, reason):
            findings.append({"level": level, "category": category, "kind": kind, "reason": reason})
        abis = sorted({a for r in result for a in r["abis"]})
        if not abis:
            finding("convertible", "conversion", "java_only", "No packaged native libraries; DEX and resources still need transformation checks.")
        elif any(a in abis for a in ("armeabi", "armeabi-v7a")):
            finding("convertible", "conversion", "arm32", "ARM32 libraries identified for guest translation.")
            if len(abis) > 1:
                finding("warning", "conversion", "multi_abi", f"Multiple native ABIs: {', '.join(abis)}; conversion must select a consistent ARM32 set.")
        else:
            finding("unsupported", "conversion", "no_arm32", f"No ARM32 libraries; packaged ABIs: {', '.join(abis)}.")
        if any(r["manifest"]["shared_user_id"] for r in result):
            finding("unsupported", "install_signing", "shared_uid", "Re-signing cannot preserve the source shared UID/signing identity.")
        if any(r["manifest"]["native_activity"] for r in result):
            finding("unsupported", "runtime", "native_activity", "NativeActivity bootstrap is not implemented; it is a later runtime step.")
        if any(r["manifest"]["is_feature_split"] == "true" for r in result):
            finding("warning", "runtime", "feature_split", "Dynamic feature split startup/resources need later integration proof.")
        if any(r["embedded_apks"] for r in result):
            finding("warning", "runtime", "embedded_apk", "Embedded APKs were inventoried only; nested installation is unsupported.")
        if any(any(p["protection_level"] and ("signature" in p["protection_level"] or
                   (p["protection_level"].isdigit() and int(p["protection_level"]) & 0xf == 2))
                   for p in r["manifest"]["permissions_declared"])
               for r in result):
            finding("warning", "install_signing", "signature_permission", "Signature permission relationships may break after re-signing.")
        if any(r["manifest"]["features"] for r in result):
            finding("warning", "runtime", "declared_features", "Review graphics/device declarations against target capabilities.")
        if any(lib["compressed"] or not lib["aligned_4096"] for r in result for lib in r["libraries"]):
            finding("warning", "conversion", "native_layout", "Some native libraries are compressed or not 4096-byte aligned; conversion must repack them.")
        finding("warning", "install_signing", "signing_identity", "Conversion re-signs with a personal key; original-signer updates and certificate-bound APIs may fail.")
        return {"status": "unsupported" if any(f["level"] == "unsupported" for f in findings) else "analyzed",
                "package": identity[0], "version_code": identity[1], "split_names": sorted(names),
                "files": result, "findings": findings}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--apksigner", default=os.environ.get("APKSIGNER", "apksigner"))
    parser.add_argument("--readelf", default=os.environ.get("READELF", "readelf"))
    args = parser.parse_args()
    try:
        report = analyze(args.inputs, args.apksigner, args.readelf)
    except (Invalid, OSError, KeyError) as e:
        report = {"status": "invalid", "error": str(e)}
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "analyzed" else 2


if __name__ == "__main__":
    sys.exit(main())
