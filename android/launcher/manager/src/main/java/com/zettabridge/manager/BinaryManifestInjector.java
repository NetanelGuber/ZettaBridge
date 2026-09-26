package com.zettabridge.manager;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Bounded binary-AXML edit used by the in-app converter. */
final class BinaryManifestInjector {
    static final String ANDROID_URI = "http://schemas.android.com/apk/res/android";
    static final long NO_INDEX = 0xffffffffL;
    static final int MAX_PROCESSES = 8;

    static final class Change {
        final String kind;
        final String name;
        final String authority;
        final String process;
        final String value;
        Change(String kind, String name, String authority, String process) {
            this(kind, name, authority, process, null);
        }
        Change(String kind, String name, String authority, String process, String value) {
            this.kind = kind;
            this.name = name;
            this.authority = authority;
            this.process = process;
            this.value = value;
        }
    }

    static final class Result {
        final byte[] manifest;
        final List<Change> changes;
        Result(byte[] manifest, List<Change> changes) {
            this.manifest = manifest;
            this.changes = changes;
        }
    }

    private static final class Chunk {
        final int offset;
        final int type;
        final int headerSize;
        final byte[] bytes;
        Chunk(int offset, int type, int headerSize, byte[] bytes) {
            this.offset = offset;
            this.type = type;
            this.headerSize = headerSize;
            this.bytes = bytes;
        }
    }

    private BinaryManifestInjector() {}

    static Result inject(byte[] input, String packageName, List<String> extraProcesses,
                         int minSdkVersion, int targetSdkVersion) throws IOException {
        if (input == null || input.length < 8 || u16(input, 0) != 3 || u16(input, 2) != 8
                || u32(input, 4) != input.length) throw new IOException("compiled binary manifest required");
        if (minSdkVersion < 1 || targetSdkVersion < minSdkVersion)
            throw new IOException("converted APK target SDK must be at least its minimum SDK");
        List<Chunk> chunks = chunks(input);
        Chunk pool = null;
        Chunk resourceMap = null;
        int applicationStart = -1;
        int applicationEnd = -1;
        int usesSdkStart = -1;
        List<String> stack = new ArrayList<>();
        List<String> strings = null;
        for (Chunk chunk : chunks) {
            if (chunk.type == 1) {
                if (pool != null) throw new IOException("multiple manifest string pools");
                if (u32(chunk.bytes, 12) != 0) throw new IOException("styled manifest string pools are unsupported");
                pool = chunk;
                strings = readStringPool(chunk.bytes);
            } else if (chunk.type == 0x180) {
                if (resourceMap != null) throw new IOException("multiple manifest resource maps");
                resourceMap = chunk;
            } else if (chunk.type == 0x102) {
                if (strings == null || chunk.headerSize != 16 || chunk.bytes.length < 36)
                    throw new IOException("invalid binary manifest element");
                String tag = lookup(strings, u32(chunk.bytes, 20));
                if (stack.isEmpty() && !"manifest".equals(tag)) throw new IOException("unexpected manifest root");
                if (stack.size() == 1 && "uses-sdk".equals(tag)) {
                    if (usesSdkStart >= 0) throw new IOException("multiple uses-sdk elements");
                    usesSdkStart = chunk.offset;
                }
                if (stack.size() == 1 && "application".equals(tag)) {
                    if (applicationStart >= 0) throw new IOException("multiple application elements");
                    applicationStart = chunk.offset;
                }
                stack.add(tag);
            } else if (chunk.type == 0x103) {
                if (strings == null || stack.isEmpty()) throw new IOException("unbalanced binary manifest");
                String tag = lookup(strings, u32(chunk.bytes, 20));
                String opened = stack.remove(stack.size() - 1);
                if (!opened.equals(tag)) throw new IOException("unbalanced binary manifest");
                if (stack.size() == 1 && "application".equals(opened)) applicationEnd = chunk.offset;
            }
        }
        if (pool == null || strings == null || !stack.isEmpty() || applicationStart < 0 || applicationEnd < 0)
            throw new IOException("manifest application element is missing or incomplete");
        String manifestPackage = findPackage(chunks, strings);
        if (!packageName.equals(manifestPackage)) throw new IOException("manifest package changed during conversion");
        if (strings.contains(ANDROID_URI) == false) throw new IOException("manifest lacks Android namespace");

        List<String> processes = new ArrayList<>();
        processes.add(null);
        Set<String> uniqueProcesses = new HashSet<>();
        uniqueProcesses.add("<default>");
        for (String process : extraProcesses) {
            if (process == null || process.isEmpty() || !uniqueProcesses.add(process)) continue;
            processes.add(process);
        }
        if (processes.size() > MAX_PROCESSES) throw new IOException("more than 8 app processes need bootstrap providers");

        List<String> authorities = new ArrayList<>();
        for (int i = 0; i < processes.size(); i++)
            authorities.add(packageName + ".zettabridge.bootstrap" + (i == 0 ? "" : "." + i));
        checkProviderCollisions(chunks, strings, authorities);

        for (String key : Arrays.asList("name", "exported", "process", "authorities", "initOrder",
                "extractNativeLibs", "minSdkVersion", "targetSdkVersion")) {
            if (count(strings, key) > 1) throw new IOException("ambiguous manifest string: " + key);
        }
        addIfMissing(strings, "uses-sdk");
        addIfMissing(strings, "minSdkVersion");
        addIfMissing(strings, "targetSdkVersion");
        for (int i = 0; i < processes.size(); i++) {
            addIfMissing(strings, "provider");
            addIfMissing(strings, bootstrapClass(i));
            addIfMissing(strings, authorities.get(i));
            if (processes.get(i) != null) addIfMissing(strings, processes.get(i));
        }
        for (String key : Arrays.asList("name", "exported", "process", "authorities", "initOrder",
                "extractNativeLibs", "minSdkVersion", "targetSdkVersion"))
            addIfMissing(strings, key);
        int nsIndex = strings.indexOf(ANDROID_URI);
        Map<String, Integer> index = new HashMap<>();
        for (int i = 0; i < strings.size(); i++) index.put(strings.get(i), i);
        int[] ids = readResourceMap(resourceMap, strings.size());
        putResourceId(ids, index.get("name"), 16842755);
        putResourceId(ids, index.get("exported"), 16842768);
        putResourceId(ids, index.get("process"), 16842769);
        putResourceId(ids, index.get("authorities"), 16842776);
        putResourceId(ids, index.get("initOrder"), 16842778);
        putResourceId(ids, index.get("extractNativeLibs"), 16844010);
        putResourceId(ids, index.get("minSdkVersion"), 16843276);
        putResourceId(ids, index.get("targetSdkVersion"), 16843376);

        ByteArrayOutputStream output = new ByteArrayOutputStream(input.length + 4096);
        output.write(input, 0, 8);
        List<Change> changes = new ArrayList<>();
        changes.add(new Change("min_sdk_version", "android:minSdkVersion", null, null,
                Integer.toString(minSdkVersion)));
        changes.add(new Change("target_sdk_version", "android:targetSdkVersion", null, null,
                Integer.toString(targetSdkVersion)));
        boolean insertedMap = false;
        for (Chunk chunk : chunks) {
            if (chunk.type == 1) {
                output.write(makeStringPool(strings));
                if (resourceMap == null) {
                    output.write(makeResourceMap(ids));
                    insertedMap = true;
                }
            } else if (chunk.type == 0x180) {
                output.write(makeResourceMap(ids));
                insertedMap = true;
            } else if (chunk.offset == usesSdkStart) {
                byte[] updated = setSdkVersion(chunk.bytes, nsIndex, index.get("minSdkVersion"), minSdkVersion,
                        "android:minSdkVersion");
                updated = setSdkVersion(updated, nsIndex, index.get("targetSdkVersion"), targetSdkVersion,
                        "android:targetSdkVersion");
                output.write(updated);
            } else if (chunk.offset == applicationStart) {
                if (usesSdkStart < 0) output.write(usesSdkChunks(index, minSdkVersion, targetSdkVersion));
                byte[] updated = setExtractNativeLibs(chunk.bytes, nsIndex, index.get("extractNativeLibs"));
                output.write(updated);
                for (int i = 0; i < processes.size(); i++) {
                    output.write(providerChunks(index, authorities, processes, i));
                    changes.add(new Change("provider", bootstrapClass(i), authorities.get(i), processes.get(i)));
                }
                changes.add(0, new Change("application_attribute", "android:extractNativeLibs", null, null));
            } else {
                output.write(chunk.bytes);
            }
        }
        if (!insertedMap) throw new IOException("manifest resource map was not written");
        byte[] result = output.toByteArray();
        put32(result, 4, result.length);
        if (u32(result, 4) != result.length) throw new IOException("manifest rewrite failed validation");
        verify(result, packageName, processes, authorities, minSdkVersion, targetSdkVersion);
        return new Result(result, changes);
    }

    static String bootstrapClass(int index) {
        return "com.zettabridge.bootstrap.BootstrapProvider" + (index == 0 ? "" : index);
    }

    private static void checkProviderCollisions(List<Chunk> chunks, List<String> strings, List<String> authorities)
            throws IOException {
        boolean inApplication = false;
        for (Chunk chunk : chunks) {
            if (chunk.type == 0x102) {
                String tag = lookup(strings, u32(chunk.bytes, 20));
                if ("application".equals(tag)) inApplication = true;
                if (inApplication && "provider".equals(tag)) {
                    Map<String, String> attributes = attributes(chunk.bytes, strings);
                    String name = attributes.get("{" + ANDROID_URI + "}name");
                    String uri = attributes.get("{" + ANDROID_URI + "}authorities");
                    if (name != null && name.startsWith("com.zettabridge.bootstrap.BootstrapProvider"))
                        throw new IOException("source already contains a ZettaBridge bootstrap provider");
                    if (uri != null) for (String authority : authorities)
                        if (Arrays.asList(uri.split(";")).contains(authority))
                            throw new IOException("bootstrap provider authority collision");
                }
            } else if (chunk.type == 0x103 && inApplication
                    && "application".equals(lookup(strings, u32(chunk.bytes, 20)))) {
                inApplication = false;
            }
        }
    }

    private static Map<String, String> attributes(byte[] element, List<String> strings) throws IOException {
        Map<String, String> result = new HashMap<>();
        int count = u16(element, 28), size = u16(element, 26), base = 16 + u16(element, 24);
        if (size < 20 || count > 4096 || base + (long) count * size > element.length)
            throw new IOException("invalid manifest attributes");
        for (int i = 0; i < count; i++) {
            int at = base + i * size;
            long ns = u32(element, at), name = u32(element, at + 4), raw = u32(element, at + 8);
            String key = (ns == NO_INDEX ? "" : "{" + lookup(strings, ns) + "}") + lookup(strings, name);
            long type = element[at + 15] & 0xff;
            long val = u32(element, at + 16);
            String value = raw != NO_INDEX ? lookup(strings, raw)
                    : type == 3 ? lookup(strings, val)
                    : type == 18 ? (val == 0 ? "false" : "true")
                    : type == 0x10 ? Long.toString(val) : "";
            if (result.put(key, value) != null) throw new IOException("duplicate manifest attribute");
        }
        return result;
    }

    private static String findPackage(List<Chunk> chunks, List<String> strings) throws IOException {
        for (Chunk chunk : chunks) if (chunk.type == 0x102 && "manifest".equals(lookup(strings, u32(chunk.bytes, 20))))
            return attributes(chunk.bytes, strings).get("package");
        return null;
    }

    static void verifyInjected(byte[] data, String packageName, List<String> processes,
                               int minSdkVersion, int targetSdkVersion) throws IOException {
        List<String> authorities = new ArrayList<>();
        for (int i = 0; i < processes.size(); i++)
            authorities.add(packageName + ".zettabridge.bootstrap" + (i == 0 ? "" : "." + i));
        verify(data, packageName, processes, authorities, minSdkVersion, targetSdkVersion);
    }

    private static void verify(byte[] data, String packageName, List<String> processes,
                               List<String> authorities, int minSdkVersion, int targetSdkVersion) throws IOException {
        List<Chunk> chunks = chunks(data);
        List<String> strings = null;
        boolean inApplication = false;
        boolean extractNativeLibs = false;
        Map<String, Map<String, String>> found = new HashMap<>();
        for (Chunk chunk : chunks) {
            if (chunk.type == 1) strings = readStringPool(chunk.bytes);
            else if (chunk.type == 0x102 && strings != null) {
                String tag = lookup(strings, u32(chunk.bytes, 20));
                if ("application".equals(tag)) {
                    inApplication = true;
                    String value = attributes(chunk.bytes, strings).get("{" + ANDROID_URI + "}extractNativeLibs");
                    extractNativeLibs = "true".equals(value);
                } else if (inApplication && "provider".equals(tag)) {
                    Map<String, String> attrs = attributes(chunk.bytes, strings);
                    String name = attrs.get("{" + ANDROID_URI + "}name");
                    for (int i = 0; i < processes.size(); i++) {
                        if (bootstrapClass(i).equals(name)) found.put(name, attrs);
                    }
                }
            } else if (chunk.type == 0x103 && inApplication && strings != null
                    && "application".equals(lookup(strings, u32(chunk.bytes, 20)))) {
                inApplication = false;
            }
        }
        if (!packageName.equals(findPackage(chunks, strings)) || !extractNativeLibs
                || found.size() != processes.size()) throw new IOException("rewritten manifest failed structural verification");
        verifyMinSdkVersion(chunks, strings, minSdkVersion);
        verifyTargetSdkVersion(chunks, strings, targetSdkVersion);
        for (int i = 0; i < processes.size(); i++) {
            Map<String, String> attrs = found.get(bootstrapClass(i));
            if (attrs == null || !authorities.get(i).equals(attrs.get("{" + ANDROID_URI + "}authorities"))
                    || !"false".equals(attrs.get("{" + ANDROID_URI + "}exported")))
                throw new IOException("injected bootstrap provider failed verification");
            String process = attrs.get("{" + ANDROID_URI + "}process");
            if (processes.get(i) != null && !processes.get(i).equals(process))
                throw new IOException("bootstrap process mapping failed verification");
            if (processes.get(i) == null && process != null)
                throw new IOException("default bootstrap provider process changed");
        }
    }

    static String rootAttribute(byte[] manifest, String attribute) throws IOException {
        if (manifest == null || manifest.length < 8 || u16(manifest, 0) != 3)
            throw new IOException("compiled binary manifest required");
        List<Chunk> chunks = chunks(manifest);
        List<String> strings = null;
        for (Chunk chunk : chunks) {
            if (chunk.type == 1) {
                if (strings != null) throw new IOException("multiple manifest string pools");
                strings = readStringPool(chunk.bytes);
            } else if (chunk.type == 0x102 && strings != null
                    && "manifest".equals(lookup(strings, u32(chunk.bytes, 20)))) {
                return attributes(chunk.bytes, strings).get(attribute);
            }
        }
        throw new IOException("manifest root is missing");
    }

    private static byte[] setExtractNativeLibs(byte[] source, int ns, int name) throws IOException {
        byte[] out = source.clone();
        int count = u16(out, 28), size = u16(out, 26), base = 16 + u16(out, 24);
        if (size != 20 || count > 4096 || base + (long) count * size > out.length)
            throw new IOException("unsupported application attribute layout");
        for (int i = 0; i < count; i++) {
            int at = base + i * size;
            if (u32(out, at) == (ns & 0xffffffffL) && u32(out, at + 4) == (name & 0xffffffffL)) {
                putAttributeValue(out, at, NO_INDEX, 18, 1);
                return out;
            }
        }
        if (base + (long) count * size != out.length) throw new IOException("application extension data unsupported");
        byte[] extra = attribute(ns, name, NO_INDEX, 18, 1);
        byte[] grown = Arrays.copyOf(out, out.length + extra.length);
        System.arraycopy(extra, 0, grown, out.length, extra.length);
        put16(grown, 28, count + 1);
        put32(grown, 4, grown.length);
        return grown;
    }

    private static byte[] setSdkVersion(byte[] source, int ns, int name, int sdkVersion,
                                        String attributeName) throws IOException {
        byte[] out = source.clone();
        int count = u16(out, 28), size = u16(out, 26), base = 16 + u16(out, 24);
        if (size != 20 || count > 4096 || base + (long) count * size > out.length)
            throw new IOException("unsupported uses-sdk attribute layout");
        int found = -1;
        for (int i = 0; i < count; i++) {
            int at = base + i * size;
            if (u32(out, at) == (ns & 0xffffffffL) && u32(out, at + 4) == (name & 0xffffffffL)) {
                if (found >= 0) throw new IOException("duplicate android:minSdkVersion attribute");
                found = at;
            }
        }
        if (found >= 0) {
            putAttributeValue(out, found, NO_INDEX, 0x10, sdkVersion);
            return out;
        }
        if (base + (long) count * size != out.length)
            throw new IOException("uses-sdk extension data unsupported while adding " + attributeName);
        byte[] extra = attribute(ns, name, NO_INDEX, 0x10, sdkVersion);
        byte[] grown = Arrays.copyOf(out, out.length + extra.length);
        System.arraycopy(extra, 0, grown, out.length, extra.length);
        put16(grown, 28, count + 1);
        put32(grown, 4, grown.length);
        return grown;
    }

    private static byte[] usesSdkChunks(Map<String, Integer> idx, int minSdkVersion, int targetSdkVersion) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(80);
        int usesSdk = idx.get("uses-sdk");
        int ns = idx.get(ANDROID_URI);
        int minSdk = idx.get("minSdkVersion"), targetSdk = idx.get("targetSdkVersion");
        ByteArrayOutputStream attributesOut = new ByteArrayOutputStream(40);
        attributesOut.write(attribute(ns, minSdk, NO_INDEX, 0x10, minSdkVersion));
        attributesOut.write(attribute(ns, targetSdk, NO_INDEX, 0x10, targetSdkVersion));
        byte[] attributes = attributesOut.toByteArray();
        write16(out, 0x102); write16(out, 16); write32(out, 36 + attributes.length);
        write32(out, 0); write32(out, NO_INDEX); write32(out, NO_INDEX); write32(out, usesSdk);
        write16(out, 20); write16(out, 20); write16(out, 2); write16(out, 0); write16(out, 0); write16(out, 0);
        out.write(attributes);
        write16(out, 0x103); write16(out, 16); write32(out, 24);
        write32(out, 0); write32(out, NO_INDEX); write32(out, NO_INDEX); write32(out, usesSdk);
        return out.toByteArray();
    }

    private static void verifyMinSdkVersion(List<Chunk> chunks, List<String> strings, int minSdkVersion)
            throws IOException {
        List<String> stack = new ArrayList<>();
        String value = null;
        for (Chunk chunk : chunks) {
            if (chunk.type == 0x102) {
                String tag = lookup(strings, u32(chunk.bytes, 20));
                if (stack.size() == 1 && "uses-sdk".equals(tag)) {
                    if (value != null) throw new IOException("multiple uses-sdk elements after rewrite");
                    value = attributes(chunk.bytes, strings).get("{" + ANDROID_URI + "}minSdkVersion");
                }
                stack.add(tag);
            } else if (chunk.type == 0x103 && !stack.isEmpty()) {
                stack.remove(stack.size() - 1);
            }
        }
        if (!Integer.toString(minSdkVersion).equals(value))
            throw new IOException("rewritten manifest minimum SDK failed verification");
    }

    private static void verifyTargetSdkVersion(List<Chunk> chunks, List<String> strings, int targetSdkVersion)
            throws IOException {
        List<String> stack = new ArrayList<>();
        String value = null;
        for (Chunk chunk : chunks) {
            if (chunk.type == 0x102) {
                String tag = lookup(strings, u32(chunk.bytes, 20));
                if (stack.size() == 1 && "uses-sdk".equals(tag)) {
                    if (value != null) throw new IOException("multiple uses-sdk elements after rewrite");
                    value = attributes(chunk.bytes, strings).get("{" + ANDROID_URI + "}targetSdkVersion");
                }
                stack.add(tag);
            } else if (chunk.type == 0x103 && !stack.isEmpty()) {
                stack.remove(stack.size() - 1);
            }
        }
        if (!Integer.toString(targetSdkVersion).equals(value))
            throw new IOException("rewritten manifest target SDK failed verification");
    }

    private static byte[] providerChunks(Map<String, Integer> idx, List<String> authorities,
                                         List<String> processes, int i) throws IOException {
        int ns = idx.get(ANDROID_URI);
        ByteArrayOutputStream attrs = new ByteArrayOutputStream();
        attrs.write(attribute(ns, idx.get("name"), idx.get(bootstrapClass(i)), 3, idx.get(bootstrapClass(i))));
        attrs.write(attribute(ns, idx.get("exported"), NO_INDEX, 18, 0));
        if (processes.get(i) != null)
            attrs.write(attribute(ns, idx.get("process"), idx.get(processes.get(i)), 3, idx.get(processes.get(i))));
        attrs.write(attribute(ns, idx.get("authorities"), idx.get(authorities.get(i)), 3, idx.get(authorities.get(i))));
        attrs.write(attribute(ns, idx.get("initOrder"), NO_INDEX, 16, 0x7fffffff));
        byte[] a = attrs.toByteArray();
        ByteArrayOutputStream out = new ByteArrayOutputStream(60 + a.length);
        write16(out, 0x102); write16(out, 16); write32(out, 36 + a.length);
        write32(out, 0); write32(out, 0xffffffffL); write32(out, 0xffffffffL);
        write32(out, idx.get("provider")); write16(out, 20); write16(out, 20);
        write16(out, processes.get(i) == null ? 4 : 5); write16(out, 0); write16(out, 0); write16(out, 0);
        out.write(a);
        write16(out, 0x103); write16(out, 16); write32(out, 24);
        write32(out, 0); write32(out, 0xffffffffL); write32(out, 0xffffffffL); write32(out, idx.get("provider"));
        return out.toByteArray();
    }

    private static byte[] attribute(int ns, int name, long raw, int type, long value) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(20);
        write32(out, ns); write32(out, name); write32(out, raw); write16(out, 8);
        out.write(0); out.write(type); write32(out, value);
        return out.toByteArray();
    }

    private static void putAttributeValue(byte[] data, int at, long raw, int type, long value) {
        put32(data, at + 8, raw); put16(data, at + 12, 8); data[at + 14] = 0; data[at + 15] = (byte) type;
        put32(data, at + 16, value);
    }

    private static List<Chunk> chunks(byte[] data) throws IOException {
        List<Chunk> result = new ArrayList<>();
        int pos = 8;
        while (pos < data.length) {
            if (pos + 8 > data.length) throw new IOException("truncated binary XML chunk");
            int type = u16(data, pos), header = u16(data, pos + 2);
            long size = u32(data, pos + 4);
            if (header < 8 || size < header || size > data.length - pos) throw new IOException("invalid binary XML chunk");
            result.add(new Chunk(pos, type, header, Arrays.copyOfRange(data, pos, pos + (int) size)));
            pos += (int) size;
        }
        return result;
    }

    private static List<String> readStringPool(byte[] chunk) throws IOException {
        int count = (int) u32(chunk, 8), flags = (int) u32(chunk, 16), stringsStart = (int) u32(chunk, 20);
        int header = u16(chunk, 2);
        if (count < 0 || count > 200000 || header < 28 || stringsStart > chunk.length
                || header + (long) count * 4 > chunk.length) throw new IOException("invalid manifest string pool");
        List<String> result = new ArrayList<>(count);
        boolean utf8 = (flags & 0x100) != 0;
        for (int i = 0; i < count; i++) {
            long relative = u32(chunk, header + i * 4);
            long startLong = stringsStart + relative;
            if (startLong >= chunk.length) throw new IOException("manifest string outside pool");
            int at = (int) startLong;
            if (utf8) {
                int[] first = length8(chunk, at); at += first[1];
                int[] second = length8(chunk, at); at += second[1];
                if (second[0] > chunk.length - at || at + second[0] >= chunk.length || chunk[at + second[0]] != 0)
                    throw new IOException("truncated UTF-8 manifest string");
                result.add(new String(chunk, at, second[0], StandardCharsets.UTF_8));
            } else {
                int[] length = length16(chunk, at); at += length[1];
                long bytes = (long) length[0] * 2;
                if (bytes > chunk.length - at || at + bytes + 2 > chunk.length || u16(chunk, at + (int) bytes) != 0)
                    throw new IOException("truncated UTF-16 manifest string");
                result.add(new String(chunk, at, (int) bytes, StandardCharsets.UTF_16LE));
            }
        }
        return result;
    }

    private static int[] length8(byte[] data, int at) throws IOException {
        if (at >= data.length) throw new IOException("truncated string length");
        int first = data[at] & 255;
        if ((first & 0x80) == 0) return new int[]{first, 1};
        if (at + 1 >= data.length) throw new IOException("truncated string length");
        return new int[]{((first & 0x7f) << 8) | (data[at + 1] & 255), 2};
    }

    private static int[] length16(byte[] data, int at) throws IOException {
        int first = u16(data, at);
        if ((first & 0x8000) == 0) return new int[]{first, 2};
        return new int[]{((first & 0x7fff) << 16) | u16(data, at + 2), 4};
    }

    private static byte[] makeStringPool(List<String> strings) throws IOException {
        List<Integer> offsets = new ArrayList<>(strings.size());
        ByteArrayOutputStream payload = new ByteArrayOutputStream();
        for (String value : strings) {
            byte[] utf8 = value.getBytes(StandardCharsets.UTF_8);
            int utf16Length = value.length();
            offsets.add(payload.size());
            writeLength8(payload, utf16Length); writeLength8(payload, utf8.length);
            payload.write(utf8); payload.write(0);
        }
        while ((payload.size() & 3) != 0) payload.write(0);
        int start = 28 + strings.size() * 4;
        int total = start + payload.size();
        ByteArrayOutputStream out = new ByteArrayOutputStream(total);
        write16(out, 1); write16(out, 28); write32(out, total); write32(out, strings.size());
        write32(out, 0); write32(out, 0x100); write32(out, start); write32(out, 0);
        for (int offset : offsets) write32(out, offset);
        payload.writeTo(out);
        return out.toByteArray();
    }

    private static byte[] makeResourceMap(int[] ids) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(8 + ids.length * 4);
        write16(out, 0x180); write16(out, 8); write32(out, 8L + ids.length * 4L);
        for (int id : ids) write32(out, id & 0xffffffffL);
        return out.toByteArray();
    }

    private static int[] readResourceMap(Chunk map, int count) throws IOException {
        int[] ids = new int[count];
        if (map == null) return ids;
        if ((map.bytes.length - 8) % 4 != 0) throw new IOException("invalid manifest resource map");
        int existing = (map.bytes.length - 8) / 4;
        if (existing > count) throw new IOException("resource map exceeds string pool");
        for (int i = 0; i < existing; i++) ids[i] = (int) u32(map.bytes, 8 + i * 4);
        return ids;
    }

    private static void putResourceId(int[] ids, int index, int id) throws IOException {
        if (index < 0 || index >= ids.length) throw new IOException("manifest string index missing");
        if (ids[index] != 0 && ids[index] != id) throw new IOException("manifest resource ID collision");
        ids[index] = id;
    }

    private static void addIfMissing(List<String> strings, String value) { if (!strings.contains(value)) strings.add(value); }
    private static int count(List<String> values, String target) { int n = 0; for (String value : values) if (target.equals(value)) n++; return n; }
    private static String lookup(List<String> strings, long index) throws IOException {
        if (index == NO_INDEX) return "";
        if (index < 0 || index >= strings.size()) throw new IOException("manifest string index out of range");
        return strings.get((int) index);
    }
    private static int u16(byte[] data, int pos) throws IOException {
        if (pos < 0 || pos + 2 > data.length) throw new IOException("truncated binary manifest");
        return (data[pos] & 255) | ((data[pos + 1] & 255) << 8);
    }
    private static long u32(byte[] data, int pos) throws IOException {
        if (pos < 0 || pos + 4 > data.length) throw new IOException("truncated binary manifest");
        return (data[pos] & 255L) | ((data[pos + 1] & 255L) << 8) | ((data[pos + 2] & 255L) << 16)
                | ((data[pos + 3] & 255L) << 24);
    }
    private static void put16(byte[] data, int pos, int value) {
        data[pos] = (byte) value; data[pos + 1] = (byte) (value >>> 8);
    }
    private static void put32(byte[] data, int pos, long value) {
        for (int i = 0; i < 4; i++) data[pos + i] = (byte) (value >>> (8 * i));
    }
    private static void write16(ByteArrayOutputStream out, int value) throws IOException {
        out.write(value & 255); out.write((value >>> 8) & 255);
    }
    private static void write32(ByteArrayOutputStream out, long value) throws IOException {
        for (int i = 0; i < 4; i++) out.write((int) (value >>> (8 * i)) & 255);
    }
    private static void writeLength8(ByteArrayOutputStream out, int value) throws IOException {
        if (value < 0 || value > 0x7fff) throw new IOException("manifest string too long");
        if (value > 127) { out.write((value >>> 8) | 0x80); out.write(value & 255); }
        else out.write(value);
    }
}
