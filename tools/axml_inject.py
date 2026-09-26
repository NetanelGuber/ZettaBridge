#!/usr/bin/env python3
"""Narrow binary AXML edit: add one private provider and enable native extraction.

Existing element/attribute chunks are retained byte-for-byte except the application
attribute when needed. Resource IDs and string indices are updated as binary AXML,
never by searching or replacing text in the manifest blob.
"""

import struct

from apk_preflight import Invalid, binary_manifest, attr

ANDROID_URI = "http://schemas.android.com/apk/res/android"
ATTR_IDS = {"name": 16842755, "exported": 16842768, "process": 16842769,
            "authorities": 16842776, "initOrder": 16842778,
            "extractNativeLibs": 16844010}
NO_INDEX = 0xffffffff
MAX_BOOTSTRAP_PROCESSES = 8


def bootstrap_class(index):
    return "com.zettabridge.bootstrap.BootstrapProvider" + (str(index) if index else "")


def u16(data, pos):
    return struct.unpack_from("<H", data, pos)[0]


def u32(data, pos):
    return struct.unpack_from("<I", data, pos)[0]


def chunks(data):
    pos = 8
    while pos < len(data):
        if pos + 8 > len(data):
            raise Invalid("truncated binary XML chunk")
        typ, header, size = struct.unpack_from("<HHI", data, pos)
        if header < 8 or size < header or pos + size > len(data):
            raise Invalid("invalid binary XML chunk")
        yield pos, typ, header, data[pos:pos + size]
        pos += size


def length8(value):
    if value > 0x7fff:
        raise Invalid("manifest string is too long")
    return bytes([(value >> 8) | 0x80, value & 255]) if value > 127 else bytes([value])


def pool_strings(chunk):
    from apk_preflight import string_pool
    return string_pool(chunk, 0, u16(chunk, 2), len(chunk))


def make_pool(strings):
    offsets = []
    payload = bytearray()
    for value in strings:
        raw = value.encode("utf-8")
        offsets.append(len(payload))
        payload.extend(length8(len(value.encode("utf-16le")) // 2))
        payload.extend(length8(len(raw)))
        payload.extend(raw)
        payload.append(0)
    while len(payload) % 4:
        payload.append(0)
    start = 28 + len(strings) * 4
    header = struct.pack("<HHIIIIII", 1, 28, start + len(payload), len(strings),
                         0, 0x100, start, 0)
    return header + b"".join(struct.pack("<I", x) for x in offsets) + payload


def make_map(ids):
    return struct.pack("<HHI", 0x180, 8, 8 + 4 * len(ids)) + b"".join(
        struct.pack("<I", x) for x in ids)


def attribute(ns, name, raw=NO_INDEX, typ=3, value=0):
    return struct.pack("<IIIHBBI", ns, name, raw, 8, 0, typ, value)


def bootstrap_processes(root):
    """One provider in each ordinary app process; isolated services cannot access this runtime."""
    app = root.find("application")
    if app is None:
        raise Invalid("manifest lacks application")
    if attr(app, "hasCode") == "false":
        raise Invalid("android:hasCode=false prevents the injected bootstrap provider from running")
    if attr(app, "directBootAware") == "true":
        raise Invalid("direct-boot application startup cannot use the credential-protected guest bundle")
    package = root.get("package")
    default = attr(app, "process") or package
    default = package + default if default.startswith(":") else default
    processes = [None]
    seen = {default}
    for component in app:
        if component.tag not in ("activity", "activity-alias", "service", "receiver", "provider"):
            continue
        if attr(component, "directBootAware") == "true":
            raise Invalid("direct-boot component " + str(attr(component, "name")) +
                          " cannot use the credential-protected guest bundle")
        if component.tag == "provider" and attr(component, "multiprocess") == "true":
            raise Invalid("multiprocess provider " + str(attr(component, "name")) +
                          " can run outside its declared bootstrap process")
        if component.tag == "service" and (attr(component, "isolatedProcess") == "true" or
                                           attr(component, "externalService") == "true"):
            raise Invalid("isolated or external service " + str(attr(component, "name")) +
                          " cannot access the per-app guest runtime")
        # An activity-alias has no process attribute: its target activity owns the process.
        raw = attr(component, "process") if component.tag != "activity-alias" else None
        if raw is None:
            continue
        resolved = package + raw if raw.startswith(":") else raw
        if resolved not in seen:
            seen.add(resolved)
            processes.append(raw)
        if len(processes) > MAX_BOOTSTRAP_PROCESSES:
            raise Invalid("more than 8 Android processes require bootstrap providers")
    return processes


def provider_chunks(name_idx, ns_idx, attr_name_idx, attr_exported_idx, attr_auth_idx,
                    attr_order_idx, attr_process_idx, class_idx, authority_idx, process_idx):
    attrs = attribute(ns_idx, attr_name_idx, class_idx, 3, class_idx)
    attrs += attribute(ns_idx, attr_exported_idx, NO_INDEX, 18, 0)
    if process_idx is not None:
        attrs += attribute(ns_idx, attr_process_idx, process_idx, 3, process_idx)
    attrs += attribute(ns_idx, attr_auth_idx, authority_idx, 3, authority_idx)
    attrs += attribute(ns_idx, attr_order_idx, NO_INDEX, 16, 0x7fffffff)
    start = struct.pack("<HHIIIIIHHHHHH", 0x102, 16, 36 + len(attrs),
                        0, NO_INDEX, NO_INDEX, name_idx, 20, 20, 5 if process_idx is not None else 4,
                        0, 0, 0) + attrs
    end = struct.pack("<HHIIIII", 0x103, 16, 24, 0, NO_INDEX, NO_INDEX, name_idx)
    return start + end


def inject(data, package):
    if data[:2] != b"\x03\x00":
        raise Invalid("conversion requires a compiled binary AndroidManifest.xml")
    root = binary_manifest(data)
    if root.get("package") != package:
        raise Invalid("manifest package changed during staging")
    app = root.find("application")
    if app is None:
        raise Invalid("manifest lacks application")
    processes = bootstrap_processes(root)
    authorities = [package + ".zettabridge.bootstrap" + ("." + str(i) if i else "")
                   for i in range(len(processes))]
    for provider in app.findall("provider"):
        if attr(provider, "name") in (bootstrap_class(i) for i in range(MAX_BOOTSTRAP_PROCESSES)):
            raise Invalid("bootstrap provider already present")
        if any(authority in (attr(provider, "authorities") or "").split(";")
               for authority in authorities):
            raise Invalid("bootstrap provider authority collision")
    parts = list(chunks(data))
    pools = [x for x in parts if x[1] == 1]
    if len(pools) != 1 or u32(pools[0][3], 12) != 0:
        raise Invalid("unsupported styled or missing manifest string pool")
    strings = pool_strings(pools[0][3])
    if ANDROID_URI not in strings:
        raise Invalid("manifest lacks Android namespace")
    for key in (ANDROID_URI, "name", "exported", "process", "authorities", "initOrder",
                "extractNativeLibs"):
        if strings.count(key) > 1:
            raise Invalid("ambiguous manifest string: " + key)
    for value in ("provider", *(bootstrap_class(i) for i in range(len(processes))), *authorities,
                  *(process for process in processes if process is not None),
                  "name", "exported", "process", "authorities", "initOrder", "extractNativeLibs"):
        if value not in strings:
            strings.append(value)
    idx = {value: strings.index(value) for value in strings}
    maps = [x for x in parts if x[1] == 0x180]
    if len(maps) > 1:
        raise Invalid("multiple manifest resource maps")
    ids = list(struct.unpack("<" + "I" * ((len(maps[0][3]) - 8) // 4), maps[0][3][8:])) if maps else []
    ids.extend([0] * (len(strings) - len(ids)))
    for key, resource_id in ATTR_IDS.items():
        current = ids[idx[key]]
        if current not in (0, resource_id):
            raise Invalid("manifest attribute resource ID collision")
        ids[idx[key]] = resource_id
    stack = []
    app_start = app_end = None
    for pos, typ, header, blob in parts:
        if typ == 0x102:
            name_idx = u32(blob, 20)
            if name_idx >= len(strings):
                raise Invalid("invalid manifest element index")
            if not stack and strings[name_idx] != "manifest":
                raise Invalid("unexpected manifest root")
            if stack == ["manifest"] and strings[name_idx] == "application":
                if app_start is not None:
                    raise Invalid("multiple applications")
                app_start = pos
            stack.append(strings[name_idx])
        elif typ == 0x103:
            if not stack or u32(blob, 20) >= len(strings) or stack[-1] != strings[u32(blob, 20)]:
                raise Invalid("unbalanced manifest elements")
            if stack == ["manifest", "application"]:
                app_end = pos
            stack.pop()
    if app_start is None or app_end is None:
        raise Invalid("cannot locate application element")
    output = bytearray(data[:8])
    changes = []
    for pos, typ, header, blob in parts:
        if typ == 1:
            output.extend(make_pool(strings))
            if not maps:
                output.extend(make_map(ids))
        elif typ == 0x180:
            output.extend(make_map(ids))
        elif pos == app_start:
            changed = bytearray(blob)
            count = u16(changed, 28)
            size = u16(changed, 26)
            base = 16 + u16(changed, 24)
            found = False
            for i in range(count):
                at = base + i * size
                if u32(changed, at) == idx[ANDROID_URI] and u32(changed, at + 4) == idx["extractNativeLibs"]:
                    changed[at + 8:at + 20] = attribute(idx[ANDROID_URI], idx["extractNativeLibs"],
                                                          NO_INDEX, 18, 1)[8:]
                    found = True
                    break
            if not found:
                if size != 20 or base + count * size != len(changed):
                    raise Invalid("unsupported application attribute layout")
                changed.extend(attribute(idx[ANDROID_URI], idx["extractNativeLibs"], NO_INDEX, 18, 1))
                struct.pack_into("<H", changed, 28, count + 1)
                struct.pack_into("<I", changed, 4, len(changed))
            output.extend(changed)
            changes.append({"kind": "application_attribute", "name": "android:extractNativeLibs",
                            "before": attr(app, "extractNativeLibs"), "after": "true"})
            for i, process in enumerate(processes):
                output.extend(provider_chunks(idx["provider"], idx[ANDROID_URI], idx["name"],
                                              idx["exported"], idx["authorities"], idx["initOrder"],
                                              idx["process"],
                                              idx[bootstrap_class(i)],
                                              idx[authorities[i]], idx[process] if process is not None else None))
                changes.append({"kind": "provider", "class": bootstrap_class(i),
                                "authorities": authorities[i], "exported": False,
                                "process": process, "initOrder": 0x7fffffff})
        elif pos == app_end:
            output.extend(blob)
        else:
            output.extend(blob)
    struct.pack_into("<I", output, 4, len(output))
    checked = binary_manifest(bytes(output))
    if checked.get("package") != package or attr(checked.find("application"), "extractNativeLibs") != "true":
        raise Invalid("manifest rewrite did not validate")
    providers = checked.find("application").findall("provider")
    if sum(attr(p, "name") in (bootstrap_class(i) for i in range(len(processes))) for p in providers) != len(processes) or not all(
            any(attr(p, "name") == bootstrap_class(i) and
                attr(p, "authorities") == authorities[i] and attr(p, "exported") == "false" and
                attr(p, "process") == process and attr(p, "initOrder") == str(0x7fffffff)
                for p in providers) for i, process in enumerate(processes)):
        raise Invalid("injected provider did not validate")
    return bytes(output), changes
