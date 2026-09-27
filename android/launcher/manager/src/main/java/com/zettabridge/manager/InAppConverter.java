package com.zettabridge.manager;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.os.Build;

import com.android.apksig.ApkSigner;
import com.android.apksig.ApkVerifier;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.TreeMap;
import java.util.regex.Pattern;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Converts one signed APK containing ARM32 libraries on-device into the same ARM64-installable format as
 * tools/apk_convert.py. It performs no privileged operation and never edits its input.
 */
final class InAppConverter {
    private static final long MAX_APK = 1024L * 1024 * 1024;
    private static final long MAX_TOTAL = 512L * 1024 * 1024;
    private static final long MAX_ENTRY = 128L * 1024 * 1024;
    private static final long MAX_NATIVE = 64L * 1024 * 1024;
    private static final int MAX_ENTRIES = 10000;
    private static final Pattern SIGNATURE = Pattern.compile(
            "^META-INF/(?:[^/]+\\.(?:RSA|DSA|EC|SF|MF)|SIG-[^/]+)$", Pattern.CASE_INSENSITIVE);
    private static final Pattern DEX = Pattern.compile("classes(?:([2-9]|[1-9][0-9]+))?\\.dex");
    private static final Pattern LIB = Pattern.compile("lib[A-Za-z0-9_+.\\-]+\\.so");
    private static final Pattern PACKAGE = Pattern.compile(
            "[A-Za-z_][A-Za-z0-9_]*(?:\\.[A-Za-z_][A-Za-z0-9_]*)+");

    interface Progress { void update(String message); }

    static final class Result {
        final File apk;
        final String report;
        final String packageName;
        final int guestLibraryCount;
        Result(File apk, String report, String packageName, int guestLibraryCount) {
            this.apk = apk;
            this.report = report;
            this.packageName = packageName;
            this.guestLibraryCount = guestLibraryCount;
        }
    }

    static final class Library {
        final String path;
        final String abi;
        final int rank;
        final long size;
        String sha256;
        Library(String path, String abi, int rank, long size) {
            this.path = path; this.abi = abi; this.rank = rank; this.size = size;
        }
    }

    private static final class Source {
        final PackageInfo info;
        final String packageName;
        final long version;
        final int minSdk;
        final int targetSdk;
        final byte[] manifest;
        final List<String> processes;
        final Map<String, Library> selectedLibraries;
        final List<Library> allLibraries;
        final Set<String> dexNames;
        final int maxDex;
        final String sourceSigner;
        final String inputHash;
        final long uncompressedBytes;
        Source(PackageInfo info, String packageName, long version, int minSdk, int targetSdk, byte[] manifest,
               List<String> processes, Map<String, Library> libraries, List<Library> allLibraries, Set<String> dexNames,
               int maxDex, String sourceSigner, String inputHash, long uncompressedBytes) {
            this.info = info; this.packageName = packageName; this.version = version;
            this.minSdk = minSdk; this.targetSdk = targetSdk;
            this.manifest = manifest; this.processes = processes; this.selectedLibraries = libraries;
            this.allLibraries = allLibraries;
            this.dexNames = dexNames; this.maxDex = maxDex; this.sourceSigner = sourceSigner;
            this.inputHash = inputHash; this.uncompressedBytes = uncompressedBytes;
        }
    }

    private static final class RuntimeBundle {
        final List<String> paths;
        final Map<String, String> hashes;
        final byte[] bridge;
        final byte[] proxy;
        final File bootstrapApk;
        final String bootstrapHash;
        final long totalBytes;
        RuntimeBundle(List<String> paths, Map<String, String> hashes, byte[] bridge,
                      byte[] proxy, File bootstrapApk, String bootstrapHash, long totalBytes) {
            this.paths = paths; this.hashes = hashes; this.bridge = bridge; this.proxy = proxy;
            this.bootstrapApk = bootstrapApk; this.bootstrapHash = bootstrapHash; this.totalBytes = totalBytes;
        }
    }

    private final Context context;
    private final AssetManager assets;
    private final RootManager.Cancellation cancellation;
    private final Progress progress;

    InAppConverter(Context context, RootManager.Cancellation cancellation, Progress progress) {
        this.context = context.getApplicationContext();
        this.assets = context.getAssets();
        this.cancellation = cancellation;
        this.progress = progress;
    }

    static void cleanupStale(Context context) {
        File[] files = context.getCacheDir().listFiles();
        if (files == null) return;
        for (File file : files) {
            if (file.getName().startsWith("conversion-") && file.isDirectory()) deleteTree(file);
            else if (file.getName().startsWith("bootstrap-") && file.getName().endsWith(".apk")) file.delete();
        }
    }

    Result convert(File input) throws Exception {
        checkCancelled();
        if (input == null || !input.isFile() || input.length() <= 0 || input.length() > MAX_APK)
            throw new IOException("select one APK no larger than 1 GiB");
        if (!Arrays.asList(Build.SUPPORTED_64_BIT_ABIS).contains("arm64-v8a"))
            throw new IOException("in-app conversion requires an ARM64 Android device");
        progress.update("Checking APK signature, manifest, DEX and ARM32 libraries...");
        Source source = inspectSource(input);
        progress.update("Checking the packaged ZettaBridge runtime...");
        RuntimeBundle runtime = loadRuntime();
        long guestBytes = 0;
        for (Library library : source.selectedLibraries.values()) guestBytes += library.size;
        if (guestBytes > MAX_TOTAL - runtime.totalBytes) {
            runtime.bootstrapApk.delete();
            throw new IOException("runtime plus guest libraries exceed 512 MiB");
        }
        progress.update("Rewriting the manifest and packaging ARM64 runtime proxies...");
        File runDir = new File(context.getCacheDir(), "conversion-" + UUID.randomUUID());
        if (!runDir.mkdir()) {
            runtime.bootstrapApk.delete();
            throw new IOException("cannot create private conversion workspace");
        }
        File unsigned = new File(runDir, "unsigned.apk");
        File output = new File(runDir, "base.apk");
        boolean published = false;
        try {
            BinaryManifestInjector.Result manifest = BinaryManifestInjector.inject(
                    source.manifest, source.packageName, source.processes, source.minSdk, source.targetSdk);
            String runtimeVersion = runtimeVersion(runtime, source.selectedLibraries, input);
            List<String> dexEntries = bootstrapDexEntries(runtime.bootstrapApk);
            progress.update("Validating the personal APK signing key...");
            PersonalSignerStore.Material signer = PersonalSignerStore.loadOrCreate(context);
            writeUnsigned(input, unsigned, source, runtime, manifest, runtimeVersion, dexEntries);
            checkCancelled();
            progress.update("Signing and verifying the converted APK...");
            sign(unsigned, output, signer, source.minSdk);
            verifySigned(output, signer.certificate, source.minSdk);
            verifyOutput(input, output, source, runtime, manifest, dexEntries, runtimeVersion);
            if (!source.inputHash.equals(hashFile(input)))
                throw new IOException("source APK changed during conversion");
            String outputHash = hashFile(output);
            String signerHash = signer.fingerprint();
            String report = makeReport(source, runtime, manifest, dexEntries, runtimeVersion,
                    outputHash, signerHash).toString(2) + "\n";
            File reportFile = new File(runDir, "transformation.json");
            try (OutputStream stream = new FileOutputStream(reportFile)) {
                stream.write(report.getBytes(StandardCharsets.UTF_8));
                stream.flush();
                stream.close();
            }
            published = true;
            return new Result(output, report, source.packageName, source.selectedLibraries.size());
        } finally {
            unsigned.delete();
            if (!published) deleteTree(runDir);
            runtime.bootstrapApk.delete();
        }
    }

    private Source inspectSource(File input) throws Exception {
        checkCancelled();
        if (input.length() > MAX_APK) throw new IOException("APK exceeds the converter size limit");
        ApkVerifier.Result verification = new ApkVerifier.Builder(input).build().verify();
        if (!verification.isVerified())
            throw new IOException("source APK signature verification failed: " + verification.getErrors());
        List<X509Certificate> sourceCertificates = verification.getSignerCertificates();
        if (sourceCertificates == null || sourceCertificates.size() != 1)
            throw new IOException("source APK must have exactly one verified signer");

        PackageInfo info = packageInfo(input);
        if (info == null || info.applicationInfo == null || info.packageName == null)
            throw new IOException("Android cannot read the APK package metadata");
        String packageName = info.packageName;
        if (!PACKAGE.matcher(packageName).matches() || packageName.equals(context.getPackageName()))
            throw new IOException("invalid or ZettaBridge Manager package name");
        if (info.sharedUserId != null) throw new IOException("shared-UID apps are unsupported");
        String defaultProcess = info.applicationInfo.processName == null ? packageName : info.applicationInfo.processName;
        if (!packageName.equals(defaultProcess))
            throw new IOException("custom default processes are unsupported by the current installer");
        if ((info.applicationInfo.flags & ApplicationInfo.FLAG_HAS_CODE) == 0)
            throw new IOException("apps with android:hasCode=false cannot use the injected bootstrap");
        rejectUnsupportedComponents(info);
        List<String> processes = extraProcesses(info);

        byte[] manifest;
        List<Library> allLibraries = new ArrayList<>();
        Set<String> dexNames = new HashSet<>();
        long total = 0;
        int count = 0;
        try (ZipFile zip = new ZipFile(input)) {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            Set<String> names = new HashSet<>();
            manifest = null;
            while (entries.hasMoreElements()) {
                checkCancelled();
                ZipEntry entry = entries.nextElement();
                String name = entry.getName();
                validateEntryName(name, entry.isDirectory());
                if (!names.add(name)) throw new IOException("duplicate APK entry: " + name);
                if (++count > MAX_ENTRIES) throw new IOException("APK contains too many entries");
                if (entry.isDirectory()) continue;
                long size = entry.getSize();
                long compressed = entry.getCompressedSize();
                if (size < 0 || size > MAX_ENTRY) throw new IOException("invalid or oversized APK entry: " + name);
                if (entry.getMethod() != ZipEntry.STORED && entry.getMethod() != ZipEntry.DEFLATED)
                    throw new IOException("unsupported APK compression method: " + name);
                if (entry.getMethod() == ZipEntry.STORED && (entry.getCrc() < 0 || compressed != size))
                    throw new IOException("invalid stored APK entry metadata: " + name);
                if (size > 1024L * 1024 && size > 1024L * Math.max(compressed, 1))
                    throw new IOException("extreme compression ratio: " + name);
                if (size > MAX_TOTAL - total) throw new IOException("APK expands beyond the converter limit");
                total += size;
                if (name.equals("AndroidManifest.xml")) {
                    if (size > 4L * 1024 * 1024) throw new IOException("manifest is too large");
                    manifest = readEntry(zip, entry, 4 * 1024 * 1024);
                    continue;
                }
                MatcherData dex = dexEntry(name);
                if (dex != null) {
                    if (!dexNames.add(name)) throw new IOException("duplicate DEX file");
                    byte[] dexBytes = readEntry(zip, entry, (int) MAX_ENTRY);
                    if (contains(dexBytes, "Lcom/zettabridge/core/ZBridge;".getBytes(StandardCharsets.US_ASCII))
                            || contains(dexBytes, "Lcom/zettabridge/bootstrap/BootstrapProvider".getBytes(StandardCharsets.US_ASCII)))
                        throw new IOException("source DEX conflicts with the ZettaBridge bootstrap classes");
                    continue;
                }
                if (name.startsWith("assets/zb/") || name.startsWith("lib/arm64-v8a/")
                        || name.equals("assets/zb-files.txt") || name.equals("assets/zb-version.txt"))
                    throw new IOException("source APK collides with reserved runtime path: " + name);
                if (name.startsWith("lib/") && name.endsWith(".so")) {
                    String[] parts = name.split("/", -1);
                    if (parts.length != 3 || !LIB.matcher(parts[2]).matches())
                        throw new IOException("unsupported native library path: " + name);
                    String abi = parts[1];
                    if (!"armeabi".equals(abi) && !"armeabi-v7a".equals(abi) && !"x86".equals(abi))
                        throw new IOException("converter accepts ARM32 libraries and matching x86 variants only");
                    if (size > MAX_NATIVE) throw new IOException("native library exceeds 64 MiB: " + name);
                    byte[] elf = readEntry(zip, entry, (int) MAX_NATIVE);
                    if ("x86".equals(abi)) validateX86Elf(elf, name);
                    else validateArm32Elf(elf, name);
                    String libraryName = parts[2];
                    int rank = "armeabi-v7a".equals(abi) ? 1 : "armeabi".equals(abi) ? 0 : -1;
                    Library found = new Library(name, abi, rank, size);
                    found.sha256 = sha256(elf);
                    allLibraries.add(found);
                    continue;
                }
                drainEntry(zip, entry, size);
            }
        }
        if (manifest == null) throw new IOException("APK has no AndroidManifest.xml");
        if (BinaryManifestInjector.rootAttribute(manifest, "split") != null)
            throw new IOException("split APKs are not supported by the current installer");
        if (dexNames.isEmpty()) throw new IOException("base APK has no classes*.dex file");
        Map<String, Library> candidates = selectGuestLibraries(allLibraries);
        if (candidates.isEmpty()) throw new IOException("APK has no ARM32 native libraries to translate");
        int maxDex = 1;
        for (String name : dexNames) maxDex = Math.max(maxDex, dexEntry(name).number);
        long version = Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : (info.versionCode & 0xffffffffL);
        int minSdk = Math.max(26, info.applicationInfo.minSdkVersion);
        if (minSdk > Build.VERSION.SDK_INT)
            throw new IOException("APK requires Android API " + minSdk + " but this device runs API " + Build.VERSION.SDK_INT);
        int targetSdk = Math.max(minSdk, info.applicationInfo.targetSdkVersion);
        return new Source(info, packageName, version, minSdk, targetSdk, manifest,
                processes, candidates, allLibraries, dexNames,
                maxDex, sha256(sourceCertificates.get(0).getEncoded()), hashFile(input), total);
    }

    static Map<String, Library> selectGuestLibraries(List<Library> allLibraries) throws IOException {
        Map<String, Library> selected = new TreeMap<>();
        Set<String> x86Names = new HashSet<>();
        for (Library library : allLibraries) {
            String name = new File(library.path).getName();
            if ("x86".equals(library.abi)) {
                x86Names.add(name);
                continue;
            }
            if ("libzbridge.so".equals(name))
                throw new IOException("guest library conflicts with libzbridge.so");
            Library old = selected.get(name);
            if (old != null && old.rank == library.rank)
                throw new IOException("duplicate guest library at the same ABI: " + name);
            if (old == null || library.rank > old.rank) selected.put(name, library);
        }
        x86Names.removeAll(selected.keySet());
        if (!x86Names.isEmpty()) {
            List<String> unmatched = new ArrayList<>(x86Names);
            Collections.sort(unmatched);
            throw new IOException("x86 library has no ARM32 counterpart: " + String.join(", ", unmatched));
        }
        return selected;
    }

    private PackageInfo packageInfo(File apk) {
        PackageManager pm = context.getPackageManager();
        long flags = PackageManager.GET_ACTIVITIES | PackageManager.GET_SERVICES | PackageManager.GET_PROVIDERS
                | PackageManager.GET_RECEIVERS | PackageManager.GET_META_DATA | PackageManager.GET_SIGNING_CERTIFICATES;
        if (Build.VERSION.SDK_INT >= 33)
            return pm.getPackageArchiveInfo(apk.getAbsolutePath(), PackageManager.PackageInfoFlags.of(flags));
        //noinspection deprecation
        return pm.getPackageArchiveInfo(apk.getAbsolutePath(), (int) flags);
    }

    private void rejectUnsupportedComponents(PackageInfo info) throws IOException {
        if (info.activities != null) for (ActivityInfo activity : info.activities) {
            if (activity.directBootAware)
                throw new IOException("direct-boot activity is unsupported: " + activity.name);
            if ("android.app.NativeActivity".equals(activity.name)
                    || activity.metaData != null && activity.metaData.containsKey("android.app.lib_name"))
                throw new IOException("NativeActivity and pure-NDK apps are not supported yet");
        }
        if (info.services != null) for (ServiceInfo service : info.services) {
            if (service.directBootAware)
                throw new IOException("direct-boot service is unsupported: " + service.name);
            if ((service.flags & 0x2) != 0 || (service.flags & 0x4) != 0)
                throw new IOException("isolated or external services are unsupported: " + service.name);
        }
        if (info.receivers != null) for (ActivityInfo receiver : info.receivers) {
            if (receiver.directBootAware)
                throw new IOException("direct-boot receiver is unsupported: " + receiver.name);
        }
        if (info.providers != null) for (ProviderInfo provider : info.providers) {
            if (provider.directBootAware)
                throw new IOException("direct-boot provider is unsupported: " + provider.name);
            if (provider.multiprocess)
                throw new IOException("multiprocess providers are unsupported: " + provider.name);
        }
    }

    private List<String> extraProcesses(PackageInfo info) throws IOException {
        String defaultProcess = info.applicationInfo.processName == null
                ? info.packageName : info.applicationInfo.processName;
        List<String> processes = new ArrayList<>();
        Set<String> unique = new HashSet<>();
        unique.add(defaultProcess);
        addProcesses(info.packageName, info.activities, defaultProcess, unique, processes);
        addProcesses(info.packageName, info.services, defaultProcess, unique, processes);
        addProcesses(info.packageName, info.receivers, defaultProcess, unique, processes);
        addProcesses(info.packageName, info.providers, defaultProcess, unique, processes);
        if (processes.size() + 1 > BinaryManifestInjector.MAX_PROCESSES)
            throw new IOException("more than 8 Android processes need bootstrap providers");
        return processes;
    }

    private static void addProcesses(String packageName, ComponentInfo[] components, String defaultProcess,
                                     Set<String> unique, List<String> processes) {
        if (components == null) return;
        for (ComponentInfo component : components) {
            String process = component.processName;
            if (process == null || process.equals(defaultProcess)) continue;
            // PackageManager expands a manifest value such as ":worker" to
            // "package.name:worker". Serialize package-local processes back in
            // their manifest form; the Android 17 package parser rejected the
            // expanded value for the injected provider on the test device.
            String manifestProcess = process.startsWith(packageName + ":")
                    ? process.substring(packageName.length()) : process;
            if (unique.add(manifestProcess)) processes.add(manifestProcess);
        }
    }

    private static final class MatcherData {
        final int number;
        MatcherData(int number) { this.number = number; }
    }

    private static MatcherData dexEntry(String path) {
        java.util.regex.Matcher matcher = DEX.matcher(path);
        if (!matcher.matches()) return null;
        try { return new MatcherData(matcher.group(1) == null ? 1 : Integer.parseInt(matcher.group(1))); }
        catch (NumberFormatException e) { return null; }
    }

    private RuntimeBundle loadRuntime() throws Exception {
        List<String> paths = new ArrayList<>();
        Set<String> uniquePaths = new HashSet<>();
        byte[] listing = readAsset("converter/runtime-files.txt", 1024 * 1024);
        String text = new String(listing, StandardCharsets.UTF_8);
        for (String raw : text.split("\\R")) {
            if (raw.isEmpty()) continue;
            validateRuntimePath(raw);
            if (!uniquePaths.add(raw)) throw new IOException("duplicate bundled runtime path");
            paths.add(raw);
        }
        if (paths.isEmpty() || !paths.contains("zb/guest/zbhost")
                || !paths.contains("zb/guest/lib/libzbjni.so")
                || !paths.contains("zb/sysroot/system/bin/linker"))
            throw new IOException("in-app converter runtime bundle is incomplete");
        Map<String, String> hashes = new LinkedHashMap<>();
        long totalBytes = 0;
        for (String path : paths) {
            checkCancelled();
            AssetDigest digest = digestAsset("converter/runtime/" + path, MAX_ENTRY);
            totalBytes += digest.size;
            if (totalBytes > MAX_TOTAL) throw new IOException("bundled runtime exceeds converter size limit");
            hashes.put("assets/" + path, digest.sha256);
        }
        byte[] bridge = readAsset("converter/libzbridge.so", (int) MAX_NATIVE);
        byte[] proxy = readAsset("converter/libzbproxy.so", (int) MAX_NATIVE);
        validateArm64Elf(bridge, "bundled libzbridge.so");
        validateArm64Elf(proxy, "bundled libzbproxy.so");
        if (!contains(bridge, "Java_com_zettabridge_core_ZBridge_activateInstalled".getBytes(StandardCharsets.US_ASCII)))
            throw new IOException("bundled bridge lacks the installed-package bootstrap entry point");
        File bootstrap = new File(context.getCacheDir(), "bootstrap-" + UUID.randomUUID() + ".apk");
        boolean ready = false;
        try {
            copyAsset("converter/bootstrap.apk", bootstrap, 32L * 1024 * 1024);
            verifyBootstrap(bootstrap);
            String bootstrapHash = hashFile(bootstrap);
            totalBytes += bridge.length + (long) proxy.length + bootstrap.length();
            if (totalBytes > MAX_TOTAL) throw new IOException("complete ZettaBridge runtime exceeds converter size limit");
            RuntimeBundle result = new RuntimeBundle(paths, hashes, bridge, proxy, bootstrap, bootstrapHash, totalBytes);
            ready = true;
            return result;
        } finally {
            if (!ready) bootstrap.delete();
        }
    }

    private void verifyBootstrap(File bootstrapApk) throws Exception {
        try (ZipFile zip = new ZipFile(bootstrapApk)) {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            List<byte[]> dex = new ArrayList<>();
            long total = 0;
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                if (dexEntry(entry.getName()) == null) continue;
                if (entry.getSize() < 0 || entry.getSize() > MAX_ENTRY - total)
                    throw new IOException("bootstrap DEX bundle exceeds size limit");
                byte[] bytes = readEntry(zip, entry, (int) MAX_ENTRY);
                total += bytes.length;
                dex.add(bytes);
            }
            if (dex.isEmpty()) throw new IOException("bundled bootstrap APK has no DEX");
            for (String required : Arrays.asList("Lcom/zettabridge/core/ZBridge;",
                    "Lcom/zettabridge/bootstrap/BootstrapProvider;")) {
                if (!containsAny(dex, required.getBytes(StandardCharsets.US_ASCII)))
                    throw new IOException("bootstrap DEX is missing " + required);
            }
            for (int i = 0; i < BinaryManifestInjector.MAX_PROCESSES; i++) {
                String required = "L" + BinaryManifestInjector.bootstrapClass(i).replace('.', '/') + ";";
                if (!containsAny(dex, required.getBytes(StandardCharsets.US_ASCII)))
                    throw new IOException("bootstrap DEX is missing " + required);
            }
        }
    }

    private static boolean containsAny(List<byte[]> data, byte[] target) {
        for (byte[] bytes : data) if (contains(bytes, target)) return true;
        return false;
    }

    private void writeUnsigned(File input, File output, Source source, RuntimeBundle runtime,
                               BinaryManifestInjector.Result manifest, String runtimeVersion,
                               List<String> bootstrapDex) throws Exception {
        try (ZipFile sourceZip = new ZipFile(input);
             OutputStream file = new FileOutputStream(output);
             ZipOutputStream zip = new ZipOutputStream(file)) {
            zip.setLevel(6);
            Enumeration<? extends ZipEntry> entries = sourceZip.entries();
            byte[] manifestBytes = manifest.manifest;
            while (entries.hasMoreElements()) {
                checkCancelled();
                ZipEntry entry = entries.nextElement();
                String name = entry.getName();
                if (entry.isDirectory() || SIGNATURE.matcher(name).matches()) continue;
                if (name.startsWith("lib/") && name.endsWith(".so")) continue;
                ZipEntry out = new ZipEntry(name);
                if (entry.getTime() >= 0) out.setTime(entry.getTime());
                out.setMethod(entry.getMethod());
                if (entry.getMethod() == ZipEntry.STORED) {
                    if ("AndroidManifest.xml".equals(name)) {
                        CRC32 crc = new CRC32();
                        crc.update(manifestBytes);
                        out.setSize(manifestBytes.length);
                        out.setCompressedSize(manifestBytes.length);
                        out.setCrc(crc.getValue());
                    } else {
                        out.setSize(entry.getSize());
                        out.setCompressedSize(entry.getCompressedSize());
                        out.setCrc(entry.getCrc());
                    }
                }
                zip.putNextEntry(out);
                if ("AndroidManifest.xml".equals(name)) zip.write(manifestBytes);
                else copyChecked(sourceZip.getInputStream(entry), zip, entry.getSize());
                zip.closeEntry();
            }
            try (ZipFile bootstrapZip = new ZipFile(runtime.bootstrapApk)) {
                int offset = 1;
                for (String dex : bootstrapDex) {
                    checkCancelled();
                    String newDex = "classes" + (source.maxDex + offset++) + ".dex";
                    putEntry(zip, newDex, bootstrapZip.getInputStream(bootstrapZip.getEntry(dex)), MAX_ENTRY);
                }
            }
            for (String path : runtime.paths) {
                checkCancelled();
                putEntry(zip, "assets/" + path, assets.open("converter/runtime/" + path), MAX_ENTRY);
            }
            for (Map.Entry<String, Library> item : source.selectedLibraries.entrySet()) {
                checkCancelled();
                ZipEntry entry = sourceZip.getEntry(item.getValue().path);
                if (entry == null) throw new IOException("guest library disappeared during conversion");
                putEntry(zip, "assets/zb/app/lib/" + item.getKey(), sourceZip.getInputStream(entry), MAX_NATIVE);
            }
            List<String> runtimeNames = new ArrayList<>(runtime.paths);
            for (String name : source.selectedLibraries.keySet()) runtimeNames.add("zb/app/lib/" + name);
            Collections.sort(runtimeNames);
            putBytes(zip, "assets/zb-files.txt", (String.join("\n", runtimeNames) + "\n")
                    .getBytes(StandardCharsets.US_ASCII));
            putBytes(zip, "assets/zb-version.txt", (runtimeVersion + "\n").getBytes(StandardCharsets.US_ASCII));
            putBytes(zip, "lib/arm64-v8a/libzbridge.so", runtime.bridge);
            for (String name : source.selectedLibraries.keySet())
                putBytes(zip, "lib/arm64-v8a/" + name, runtime.proxy);
        }
    }

    private List<String> bootstrapDexEntries(File bootstrapApk) throws IOException {
        List<String> dex = new ArrayList<>();
        try (ZipFile zip = new ZipFile(bootstrapApk)) {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                String name = entries.nextElement().getName();
                if (dexEntry(name) != null) dex.add(name);
            }
        }
        dex.sort((a, b) -> Integer.compare(dexEntry(a).number, dexEntry(b).number));
        if (dex.isEmpty()) throw new IOException("bundled bootstrap APK has no DEX");
        return dex;
    }

    private String runtimeVersion(RuntimeBundle runtime, Map<String, Library> libraries, File sourceApk) throws Exception {
        List<String> names = new ArrayList<>();
        names.addAll(runtime.paths);
        for (String name : libraries.keySet()) names.add("zb/app/lib/" + name);
        Collections.sort(names);
        MessageDigest version = MessageDigest.getInstance("SHA-256");
        try (ZipFile zip = new ZipFile(sourceApk)) {
            for (String name : names) {
                checkCancelled();
                MessageDigest item = MessageDigest.getInstance("SHA-256");
                item.update((name + "\n").getBytes(StandardCharsets.UTF_8));
                if (name.startsWith("zb/app/lib/")) {
                    String libName = name.substring("zb/app/lib/".length());
                    ZipEntry guest = zip.getEntry(libraries.get(libName).path);
                    if (guest == null) throw new IOException("guest library disappeared during version calculation");
                    try (InputStream in = zip.getInputStream(guest)) {
                        byte[] buffer = new byte[65536]; int count;
                        while ((count = in.read(buffer)) != -1) { checkCancelled(); item.update(buffer, 0, count); }
                    }
                } else {
                    try (InputStream in = assets.open("converter/runtime/" + name)) {
                        byte[] buffer = new byte[65536]; int count;
                        while ((count = in.read(buffer)) != -1) { checkCancelled(); item.update(buffer, 0, count); }
                    }
                }
                version.update(item.digest());
            }
        }
        return hex(version.digest());
    }

    private void sign(File unsigned, File output, PersonalSignerStore.Material material, int minSdkVersion) throws Exception {
        ApkSigner.SignerConfig signer = new ApkSigner.SignerConfig.Builder(
                "zettabridge", material.privateKey, Collections.singletonList(material.certificate)).build();
        ApkSigner signerTool = new ApkSigner.Builder(Collections.singletonList(signer))
                .setInputApk(unsigned)
                .setOutputApk(output)
                .setMinSdkVersion(minSdkVersion)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                .setV4SigningEnabled(false)
                .build();
        signerTool.sign();
    }

    private void verifySigned(File apk, X509Certificate certificate, int minSdkVersion) throws Exception {
        ApkVerifier.Result result = new ApkVerifier.Builder(apk)
                .setMinCheckedPlatformVersion(minSdkVersion)
                .build().verify();
        if (!result.isVerified()) {
            String details = "allErrors=" + result.getAllErrors()
                    + "; warnings=" + result.getWarnings()
                    + "; verifiedSchemes=[v1=" + result.isVerifiedUsingV1Scheme()
                    + ",v2=" + result.isVerifiedUsingV2Scheme()
                    + ",v3=" + result.isVerifiedUsingV3Scheme()
                    + "]; signerCounts=[v1=" + result.getV1SchemeSigners().size()
                    + ",v2=" + result.getV2SchemeSigners().size()
                    + ",v3=" + result.getV3SchemeSigners().size() + "]";
            if (details.length() > 1600) details = details.substring(0, 1600) + "...";
            throw new IOException("converted APK signature verification failed: " + details);
        }
        List<X509Certificate> signers = result.getSignerCertificates();
        if (signers == null || signers.size() != 1
                || !Arrays.equals(signers.get(0).getEncoded(), certificate.getEncoded()))
            throw new IOException("converted APK signer differs from the personal key");
        try (ZipFile zip = new ZipFile(apk)) {
            if (zip.getEntry("lib/arm64-v8a/libzbridge.so") == null
                    || zip.getEntry("assets/zb-files.txt") == null)
                throw new IOException("converted APK is missing its ARM64 bridge or guest bundle index");
            Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                String name = entries.nextElement().getName();
                if (name.startsWith("lib/") && name.endsWith(".so")
                        && !name.startsWith("lib/arm64-v8a/"))
                    throw new IOException("converted APK retains a non-ARM64 host library");
            }
        }
    }

    private void verifyOutput(File sourceApk, File outputApk, Source source, RuntimeBundle runtime,
                              BinaryManifestInjector.Result manifest, List<String> bootstrapDex,
                              String runtimeVersion) throws Exception {
        Set<String> allowed = new HashSet<>();
        Set<String> expectedGenerated = new HashSet<>();
        try (ZipFile sourceZip = new ZipFile(sourceApk); ZipFile outputZip = new ZipFile(outputApk)) {
            Enumeration<? extends ZipEntry> inputEntries = sourceZip.entries();
            while (inputEntries.hasMoreElements()) {
                checkCancelled();
                ZipEntry input = inputEntries.nextElement();
                String name = input.getName();
                if (input.isDirectory() || SIGNATURE.matcher(name).matches()
                        || name.startsWith("lib/") && name.endsWith(".so")) continue;
                if (!allowed.add(name)) throw new IOException("duplicate preserved source entry: " + name);
                ZipEntry actual = outputZip.getEntry(name);
                if (actual == null) throw new IOException("converted APK lost source entry: " + name);
                if ("AndroidManifest.xml".equals(name)) {
                    byte[] outputManifest = readEntry(outputZip, actual, 4 * 1024 * 1024);
                    if (!Arrays.equals(manifest.manifest, outputManifest))
                        throw new IOException("converted manifest differs from the validated rewrite");
                    List<String> allProcesses = new ArrayList<>();
                    allProcesses.add(null);
                    allProcesses.addAll(source.processes);
                    BinaryManifestInjector.verifyInjected(outputManifest, source.packageName, allProcesses,
                            source.minSdk, source.targetSdk);
                } else if (!hashEntry(sourceZip, input).equals(hashEntry(outputZip, actual))) {
                    throw new IOException("converted APK changed preserved entry: " + name);
                }
            }

            try (ZipFile bootstrapZip = new ZipFile(runtime.bootstrapApk)) {
                for (int i = 0; i < bootstrapDex.size(); i++) {
                    String name = "classes" + (source.maxDex + i + 1) + ".dex";
                    ZipEntry original = bootstrapZip.getEntry(bootstrapDex.get(i));
                    ZipEntry actual = outputZip.getEntry(name);
                    if (original == null || actual == null || !hashEntry(bootstrapZip, original).equals(hashEntry(outputZip, actual)))
                        throw new IOException("converted bootstrap DEX failed verification");
                    expectedGenerated.add(name);
                }
            }
            for (String path : runtime.paths) {
                ZipEntry entry = outputZip.getEntry("assets/" + path);
                if (entry == null || !runtime.hashes.get("assets/" + path).equals(hashEntry(outputZip, entry)))
                    throw new IOException("converted runtime asset failed verification: " + path);
                expectedGenerated.add("assets/" + path);
            }
            for (Map.Entry<String, Library> lib : source.selectedLibraries.entrySet()) {
                String guestPath = "assets/zb/app/lib/" + lib.getKey();
                ZipEntry guest = outputZip.getEntry(guestPath);
                if (guest == null || !lib.getValue().sha256.equals(hashEntry(outputZip, guest)))
                    throw new IOException("converted guest library failed verification: " + lib.getKey());
                expectedGenerated.add(guestPath);
                String proxyPath = "lib/arm64-v8a/" + lib.getKey();
                ZipEntry proxy = outputZip.getEntry(proxyPath);
                if (proxy == null || !sha256(runtime.proxy).equals(hashEntry(outputZip, proxy)))
                    throw new IOException("converted ARM64 proxy failed verification: " + lib.getKey());
                expectedGenerated.add(proxyPath);
            }
            ZipEntry bridge = outputZip.getEntry("lib/arm64-v8a/libzbridge.so");
            if (bridge == null || !sha256(runtime.bridge).equals(hashEntry(outputZip, bridge)))
                throw new IOException("converted ARM64 bridge failed verification");
            expectedGenerated.add("lib/arm64-v8a/libzbridge.so");
            StringBuilder list = new StringBuilder();
            List<String> paths = new ArrayList<>(runtime.paths);
            for (String name : source.selectedLibraries.keySet()) paths.add("zb/app/lib/" + name);
            Collections.sort(paths);
            for (String path : paths) list.append(path).append('\n');
            String listPath = "assets/zb-files.txt";
            ZipEntry listEntry = outputZip.getEntry(listPath);
            if (listEntry == null || !Arrays.equals(list.toString().getBytes(StandardCharsets.US_ASCII),
                    readEntry(outputZip, listEntry, 1024 * 1024))) throw new IOException("guest bundle index failed verification");
            expectedGenerated.add(listPath);
            String versionPath = "assets/zb-version.txt";
            ZipEntry versionEntry = outputZip.getEntry(versionPath);
            if (versionEntry == null || !Arrays.equals((runtimeVersion + "\n").getBytes(StandardCharsets.US_ASCII),
                    readEntry(outputZip, versionEntry, 256))) throw new IOException("guest bundle version failed verification");
            expectedGenerated.add(versionPath);

            Set<String> outputNames = new HashSet<>();
            Enumeration<? extends ZipEntry> outputEntries = outputZip.entries();
            while (outputEntries.hasMoreElements()) {
                ZipEntry entry = outputEntries.nextElement();
                String name = entry.getName();
                if (!outputNames.add(name)) throw new IOException("duplicate converted APK entry: " + name);
                if (entry.isDirectory() || SIGNATURE.matcher(name).matches()) continue;
                if (!allowed.contains(name) && !expectedGenerated.contains(name))
                    throw new IOException("unexpected converted APK entry: " + name);
            }
            if (!outputNames.containsAll(allowed) || !outputNames.containsAll(expectedGenerated))
                throw new IOException("converted APK is missing verified content");
        }
    }

    private String hashEntry(ZipFile zip, ZipEntry entry) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream in = zip.getInputStream(entry)) {
            byte[] buffer = new byte[65536]; int count; long size = 0;
            while ((count = in.read(buffer)) != -1) {
                checkCancelled();
                size += count;
                if (size > MAX_ENTRY) throw new IOException("APK entry exceeds verification size limit");
                digest.update(buffer, 0, count);
            }
            if (entry.getSize() >= 0 && size != entry.getSize()) throw new IOException("APK entry size mismatch");
        }
        return hex(digest.digest());
    }

    private JSONObject makeReport(Source source, RuntimeBundle runtime,
                                  BinaryManifestInjector.Result manifest, List<String> dex,
                                  String runtimeVersion, String outputHash,
                                  String signerHash) throws Exception {
        JSONArray mapping = new JSONArray();
        for (Map.Entry<String, Library> item : source.selectedLibraries.entrySet()) {
            Library lib = item.getValue();
            mapping.put(new JSONObject().put("guest", item.getKey()).put("source", "selected APK")
                    .put("entry", lib.path).put("sha256", lib.sha256).put("abi", lib.abi));
        }
        JSONArray changes = new JSONArray();
        Set<String> selectedPaths = new HashSet<>();
        for (Library library : source.selectedLibraries.values()) selectedPaths.add(library.path);
        for (Library library : source.allLibraries) {
            if (selectedPaths.contains(library.path)) {
                changes.put(new JSONObject().put("kind", "move_guest_library").put("from", library.path)
                        .put("to", "assets/zb/app/lib/" + new File(library.path).getName()));
            } else {
                changes.put(new JSONObject().put("kind", "remove_unselected_abi_variant").put("path", library.path));
            }
        }
        for (int i = 0; i < dex.size(); i++)
            changes.put(new JSONObject().put("kind", "add_bootstrap_dex")
                    .put("path", "classes" + (source.maxDex + i + 1) + ".dex"));
        for (String name : source.selectedLibraries.keySet())
            changes.put(new JSONObject().put("kind", "add_arm64_proxy").put("path", "lib/arm64-v8a/" + name)
                    .put("guest", "assets/zb/app/lib/" + name));
        JSONArray manifestChanges = new JSONArray();
        for (BinaryManifestInjector.Change change : manifest.changes) {
            JSONObject value = new JSONObject().put("kind", change.kind);
            if ("application_attribute".equals(change.kind)) {
                value.put("name", change.name).put("after", "true");
            } else if ("min_sdk_version".equals(change.kind)
                    || "target_sdk_version".equals(change.kind)) {
                value.put("name", change.name).put("after", Integer.parseInt(change.value));
            } else {
                value.put("class", change.name).put("authorities", change.authority)
                        .put("exported", false).put("initOrder", Integer.MAX_VALUE);
                if (change.process == null) value.put("process", JSONObject.NULL);
                else value.put("process", change.process);
            }
            manifestChanges.put(value);
        }
        JSONArray files = new JSONArray().put(new JSONObject()
                .put("input", "selected.apk").put("input_sha256", source.inputHash)
                .put("output", "base.apk").put("output_sha256", outputHash).put("split", JSONObject.NULL)
                .put("changes", changes));
        JSONObject hashes = new JSONObject();
        for (String path : runtime.paths) hashes.put("assets/" + path, runtime.hashes.get("assets/" + path));
        JSONArray findings = new JSONArray();
        String applicationClass = source.info.applicationInfo.className;
        if (applicationClass != null && !"android.app.Application".equals(applicationClass)) {
            findings.put(new JSONObject().put("level", "warning").put("kind", "custom_application_startup")
                    .put("detail", "Native loads in a custom Application initializer or attachBaseContext may run before the injected provider."));
        }
        return new JSONObject().put("schema", 1).put("package", source.packageName)
                .put("version_code", Long.toString(source.version))
                .put("min_sdk_version", source.minSdk)
                .put("target_sdk_version", source.targetSdk)
                .put("source_signer_sha256", new JSONArray().put(source.sourceSigner))
                .put("output_signer_sha256", signerHash).put("guest_mapping", mapping)
                .put("runtime_assets_sha256", hashes).put("runtime_version", runtimeVersion)
                .put("bootstrap_apk_sha256", runtime.bootstrapHash)
                .put("proxy_sha256", sha256(runtime.proxy)).put("zbridge_sha256", sha256(runtime.bridge))
                .put("files", files).put("manifest_changes", manifestChanges)
                .put("resource_changes", new JSONArray()).put("preflight_findings", findings);
    }

    private static void validateArm32Elf(byte[] data, String name) throws IOException {
        if (data.length < 52 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F'
                || data[4] != 1 || data[5] != 1 || data[6] != 1 || u16(data, 16) != 3 || u16(data, 18) != 40)
            throw new IOException("native library is not a little-endian ARM32 shared ELF: " + name);
        int headerSize = u16(data, 40), phEntrySize = u16(data, 42), phCount = u16(data, 44);
        long phOffset = u32(data, 28);
        if (headerSize != 52 || phEntrySize != 32 || phCount <= 0
                || phOffset + (long) phEntrySize * phCount > data.length)
            throw new IOException("malformed ARM32 ELF program-header table: " + name);
    }

    private static void validateX86Elf(byte[] data, String name) throws IOException {
        if (data.length < 52 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F'
                || data[4] != 1 || data[5] != 1 || data[6] != 1 || u16(data, 16) != 3 || u16(data, 18) != 3)
            throw new IOException("native library is not a little-endian x86 shared ELF: " + name);
        int headerSize = u16(data, 40), phEntrySize = u16(data, 42), phCount = u16(data, 44);
        long phOffset = u32(data, 28);
        if (headerSize != 52 || phEntrySize != 32 || phCount <= 0
                || phOffset + (long) phEntrySize * phCount > data.length)
            throw new IOException("malformed x86 ELF program-header table: " + name);
    }

    private static void validateArm64Elf(byte[] data, String name) throws IOException {
        if (data.length < 64 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F'
                || data[4] != 2 || data[5] != 1 || data[6] != 1 || u16(data, 16) != 3 || u16(data, 18) != 183)
            throw new IOException(name + " is not a little-endian ARM64 shared ELF");
    }

    private static void validateEntryName(String name, boolean directory) throws IOException {
        if (name == null || name.isEmpty() || name.startsWith("/") || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0)
            throw new IOException("unsafe APK ZIP path");
        if (name.matches("^[A-Za-z]:.*")) throw new IOException("unsafe APK ZIP path");
        String normalized = directory && name.endsWith("/") ? name.substring(0, name.length() - 1) : name;
        for (String part : normalized.split("/", -1))
            if (part.isEmpty() || part.equals(".") || part.equals("..")) throw new IOException("unsafe APK ZIP path");
    }

    private static byte[] readEntry(ZipFile zip, ZipEntry entry, int limit) throws IOException {
        try (InputStream input = zip.getInputStream(entry); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[32768];
            int n;
            while ((n = input.read(buffer)) != -1) {
                if (n > limit - output.size()) throw new IOException("APK entry exceeds size limit: " + entry.getName());
                output.write(buffer, 0, n);
            }
            if (entry.getSize() >= 0 && output.size() != entry.getSize())
                throw new IOException("APK entry size mismatch: " + entry.getName());
            return output.toByteArray();
        }
    }

    private void drainEntry(ZipFile zip, ZipEntry entry, long expected) throws IOException {
        try (InputStream in = zip.getInputStream(entry)) {
            byte[] buffer = new byte[32768];
            long size = 0;
            int count;
            while ((count = in.read(buffer)) != -1) {
                checkCancelled();
                size += count;
                if (size > expected || size > MAX_ENTRY) throw new IOException("APK entry expanded beyond declared size");
            }
            if (size != expected) throw new IOException("APK entry size mismatch: " + entry.getName());
        }
    }

    private void copyChecked(InputStream in, OutputStream out, long expected) throws IOException {
        try (InputStream input = in) {
            byte[] buffer = new byte[65536];
            long size = 0;
            int n;
            while ((n = input.read(buffer)) != -1) {
                checkCancelled();
                size += n;
                if (size > expected || size > MAX_ENTRY) throw new IOException("APK entry expanded beyond declared size");
                out.write(buffer, 0, n);
            }
            if (size != expected) throw new IOException("APK entry size mismatch while copying");
        }
    }

    private void putEntry(ZipOutputStream zip, String name, InputStream input, long limit) throws IOException {
        validateEntryName(name, false);
        ZipEntry entry = new ZipEntry(name);
        zip.putNextEntry(entry);
        try (InputStream in = input) {
            byte[] buffer = new byte[65536];
            long count = 0;
            int n;
            while ((n = in.read(buffer)) != -1) {
                checkCancelled();
                count += n;
                if (count > limit || count > MAX_TOTAL) throw new IOException("generated runtime entry exceeds limit");
                zip.write(buffer, 0, n);
            }
        }
        zip.closeEntry();
    }

    private void putBytes(ZipOutputStream zip, String name, byte[] data) throws IOException {
        ZipEntry entry = new ZipEntry(name);
        zip.putNextEntry(entry);
        zip.write(data);
        zip.closeEntry();
    }

    private byte[] readAsset(String path, int limit) throws IOException {
        try (InputStream in = assets.open(path); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[32768]; int n;
            while ((n = in.read(buffer)) != -1) {
                if (n > limit - out.size()) throw new IOException("bundled converter input exceeds size limit: " + path);
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        }
    }

    private void copyAsset(String path, File target, long limit) throws IOException {
        try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[32768]; long size = 0; int n;
            while ((n = in.read(buffer)) != -1) {
                size += n;
                if (size > limit) throw new IOException("bundled converter APK exceeds size limit");
                out.write(buffer, 0, n);
            }
            out.flush();
        }
    }

    private static final class AssetDigest {
        final String sha256;
        final long size;
        AssetDigest(String sha256, long size) { this.sha256 = sha256; this.size = size; }
    }

    private AssetDigest digestAsset(String path, long limit) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        long size = 0;
        try (InputStream in = assets.open(path)) {
            byte[] buffer = new byte[65536]; int count;
            while ((count = in.read(buffer)) != -1) {
                checkCancelled();
                size += count;
                if (size > limit) throw new IOException("bundled runtime asset exceeds size limit: " + path);
                digest.update(buffer, 0, count);
            }
        }
        return new AssetDigest(hex(digest.digest()), size);
    }

    private static String hashFile(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[65536]; int n;
            while ((n = in.read(buffer)) != -1) digest.update(buffer, 0, n);
        }
        return hex(digest.digest());
    }

    private static String sha256(byte[] data) throws Exception {
        return hex(MessageDigest.getInstance("SHA-256").digest(data));
    }

    private static String hex(byte[] data) {
        StringBuilder out = new StringBuilder(data.length * 2);
        for (byte b : data) out.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return out.toString();
    }

    private static byte[] hexBytes(String hex) {
        byte[] output = new byte[hex.length() / 2];
        for (int i = 0; i < output.length; i++) output[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
        return output;
    }

    private static boolean contains(byte[] data, byte[] target) {
        outer: for (int i = 0; i <= data.length - target.length; i++) {
            for (int j = 0; j < target.length; j++) if (data[i + j] != target[j]) continue outer;
            return true;
        }
        return false;
    }

    private void validateRuntimePath(String path) throws IOException {
        if (!path.startsWith("zb/guest/") && !path.startsWith("zb/sysroot/"))
            throw new IOException("invalid bundled runtime path: " + path);
        validateEntryName(path, false);
    }

    private void checkCancelled() throws IOException {
        if (cancellation.isCancelled()) throw new IOException("conversion cancelled");
    }

    private static int u16(byte[] bytes, int offset) throws IOException {
        if (offset < 0 || offset + 2 > bytes.length) throw new IOException("truncated ELF header");
        return (bytes[offset] & 255) | ((bytes[offset + 1] & 255) << 8);
    }

    private static long u32(byte[] bytes, int offset) throws IOException {
        if (offset < 0 || offset + 4 > bytes.length) throw new IOException("truncated ELF header");
        return (bytes[offset] & 255L) | ((bytes[offset + 1] & 255L) << 8)
                | ((bytes[offset + 2] & 255L) << 16) | ((bytes[offset + 3] & 255L) << 24);
    }

    private static void deleteTree(File root) {
        if (root == null || !root.exists()) return;
        File[] children = root.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        root.delete();
    }
}
