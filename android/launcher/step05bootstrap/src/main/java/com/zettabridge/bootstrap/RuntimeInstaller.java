package com.zettabridge.bootstrap;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.FileVisitResult;

/** Bounded extraction under the installed app UID, serialized across processes. */
final class RuntimeInstaller {
    private RuntimeInstaller() {}

    static void install(Context context) throws IOException {
        File files = context.getFilesDir();
        File target = new File(files, "zb");
        String version = readVersion(context);
        try (RandomAccessFile lockFile = new RandomAccessFile(new File(files, "zb-install.lock"), "rw");
             FileLock ignored = lockFile.getChannel().lock()) {
            File marker = new File(target, ".bundle-version");
            if (marker.isFile() && version.equals(new String(Files.readAllBytes(marker.toPath()),
                    StandardCharsets.US_ASCII).trim()) && complete(target, context)) return;
            File staging = new File(files, "zb.installing");
            File backup = new File(files, "zb.previous");
            deleteTree(staging.toPath());
            if (!staging.mkdir()) throw new IOException("cannot create " + staging);
            boolean published = false;
            try {
                int count = 0;
                long total = 0;
                try (BufferedReader list = new BufferedReader(new InputStreamReader(
                        context.getAssets().open("zb-files.txt"), StandardCharsets.US_ASCII))) {
                    String asset;
                    while ((asset = list.readLine()) != null) {
                        if (!safeAssetName(asset) || ++count > 10000)
                            throw new IOException("unsafe runtime asset list");
                        File out = new File(staging, asset.substring(3));
                        if (!out.getParentFile().isDirectory() && !out.getParentFile().mkdirs())
                            throw new IOException("cannot create " + out.getParent());
                        try (InputStream in = context.getAssets().open(asset)) {
                            byte[] buffer = new byte[65536];
                            try (java.io.OutputStream output = Files.newOutputStream(out.toPath())) {
                                int n;
                                while ((n = in.read(buffer)) != -1) {
                                    total += n;
                                    if (total > 512L * 1024 * 1024) throw new IOException("runtime bundle too large");
                                    output.write(buffer, 0, n);
                                }
                            }
                        }
                        if (!out.setReadOnly()) throw new IOException("cannot make read-only " + out);
                    }
                }
                Files.write(new File(staging, ".bundle-version").toPath(),
                        (version + "\n").getBytes(StandardCharsets.US_ASCII));
                deleteTree(backup.toPath());
                if (target.exists()) Files.move(target.toPath(), backup.toPath());
                try {
                    Files.move(staging.toPath(), target.toPath());
                    published = true;
                } catch (IOException failure) {
                    if (backup.exists()) Files.move(backup.toPath(), target.toPath());
                    throw failure;
                }
                deleteTree(backup.toPath());
            } finally {
                if (!published) deleteTree(staging.toPath());
            }
        }
    }

    private static boolean complete(File root, Context context) throws IOException {
        try (BufferedReader list = new BufferedReader(new InputStreamReader(
                context.getAssets().open("zb-files.txt"), StandardCharsets.US_ASCII))) {
            String asset;
            while ((asset = list.readLine()) != null) {
                if (!safeAssetName(asset) || !new File(root, asset.substring(3)).isFile()) return false;
            }
        }
        return true;
    }

    private static String readVersion(Context context) throws IOException {
        try (InputStream in = context.getAssets().open("zb-version.txt")) {
            String version = new String(in.readNBytes(128), StandardCharsets.US_ASCII).trim();
            if (!version.matches("[0-9a-f]{64}")) throw new IOException("invalid runtime bundle version");
            return version;
        }
    }

    private static boolean safeAssetName(String asset) {
        if (!asset.matches("zb/(?:[A-Za-z0-9_.+\\-]+/)*[A-Za-z0-9_.+\\-]+")) return false;
        for (String part : asset.split("/")) {
            if (part.equals(".") || part.equals("..")) return false;
        }
        return true;
    }

    private static void deleteTree(Path root) throws IOException {
        if (!Files.exists(root)) return;
        Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                Files.delete(file);
                return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path dir, IOException error) throws IOException {
                if (error != null) throw error;
                Files.delete(dir);
                return FileVisitResult.CONTINUE;
            }
        });
    }
}
