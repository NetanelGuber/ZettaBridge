package com.zettabridge.manager;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.security.keystore.KeyPermanentlyInvalidatedException;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.PrivateKey;
import java.security.Signature;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import java.util.Calendar;
import java.util.Enumeration;
import java.util.Locale;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** Persists a personal APK signer inside the manager sandbox. */
final class PersonalSignerStore {
    private static final String GENERATED_ALIAS = "zettabridge-personal-apk-signer";
    private static final String PASSWORD_KEY_ALIAS = "zettabridge-p12-password-key";
    private static final int MAX_KEYSTORE_BYTES = 8 * 1024 * 1024;

    static final class Material {
        final PrivateKey privateKey;
        final X509Certificate certificate;
        Material(PrivateKey privateKey, X509Certificate certificate) {
            this.privateKey = privateKey; this.certificate = certificate;
        }
        String fingerprint() throws Exception {
            return hex(MessageDigest.getInstance("SHA-256").digest(certificate.getEncoded()));
        }
    }

    private PersonalSignerStore() {}

    static String importPkcs12(Context context, ContentResolver resolver, Uri uri,
                               String password, RootManager.Cancellation cancellation) throws Exception {
        if (uri == null || !"content".equals(uri.getScheme())) throw new IOException("select the PKCS#12 key through DocumentsUI");
        if (password == null || password.length() < 1) throw new IOException("key password is empty");
        File directory = new File(context.getFilesDir(), "converter-signing");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("cannot create private key storage");
        File temporary = new File(directory, "personal-import.tmp");
        File keyFile = new File(directory, "personal.p12");
        File aliasFile = new File(directory, "alias");
        File passwordFile = new File(directory, "password.enc");
        File stagedPassword = new File(directory, "password.enc.tmp");
        X509Certificate certificate;
        String alias;
        boolean keyMoved = false;
        try {
            try (InputStream in = resolver.openInputStream(uri); FileOutputStream out = new FileOutputStream(temporary)) {
                if (in == null) throw new IOException("selected key file cannot be opened");
                byte[] buffer = new byte[32768]; long size = 0; int n;
                while ((n = in.read(buffer)) != -1) {
                    if (cancellation.isCancelled()) throw new IOException("key import cancelled");
                    size += n;
                    if (size > MAX_KEYSTORE_BYTES) throw new IOException("PKCS#12 file exceeds 8 MiB");
                    out.write(buffer, 0, n);
                }
                if (size == 0) throw new IOException("selected key file is empty");
                out.flush();
            }
            char[] secret = password.toCharArray();
            try {
                KeyStore imported = KeyStore.getInstance("PKCS12");
                try (InputStream in = new FileInputStream(temporary)) { imported.load(in, secret); }
                alias = findAlias(imported);
                KeyStore.Entry entry = imported.getEntry(alias, new KeyStore.PasswordProtection(secret));
                if (!(entry instanceof KeyStore.PrivateKeyEntry)) throw new IOException("PKCS#12 alias does not contain a private key");
                KeyStore.PrivateKeyEntry privateEntry = (KeyStore.PrivateKeyEntry) entry;
                if (!(privateEntry.getCertificate() instanceof X509Certificate)) throw new IOException("PKCS#12 certificate is not X.509");
                certificate = (X509Certificate) privateEntry.getCertificate();
                if (!"RSA".equalsIgnoreCase(privateEntry.getPrivateKey().getAlgorithm())
                        || !"RSA".equalsIgnoreCase(certificate.getPublicKey().getAlgorithm())
                        || certificate.getPublicKey().getEncoded().length < 256)
                    throw new IOException("personal APK signer must use an RSA key of at least 2048 bits");
                certificate.checkValidity();
                String fingerprint = fingerprint(certificate);
                requireAnchoredSigner(context, fingerprint);
                writeEncryptedPassword(context, stagedPassword, secret);
                Files.move(temporary.toPath(), keyFile.toPath(), StandardCopyOption.REPLACE_EXISTING,
                        StandardCopyOption.ATOMIC_MOVE);
                keyMoved = true;
                Files.write(aliasFile.toPath(), alias.getBytes(StandardCharsets.UTF_8));
                Files.move(stagedPassword.toPath(), passwordFile.toPath(), StandardCopyOption.REPLACE_EXISTING,
                        StandardCopyOption.ATOMIC_MOVE);
                removeGeneratedKey();
                return fingerprint;
            } finally {
                Arrays.fill(secret, '\0');
            }
        } finally {
            temporary.delete();
            stagedPassword.delete();
            if (!keyMoved && temporary.exists()) temporary.delete();
        }
    }

    static Material loadOrCreate(Context context) throws Exception {
        File directory = new File(context.getFilesDir(), "converter-signing");
        File keyFile = new File(directory, "personal.p12");
        if (keyFile.isFile()) {
            Material imported = loadImported(directory);
            selfTest(imported);
            requireAnchoredSigner(context, imported.fingerprint());
            return imported;
        }

        Material material;
        try {
            material = loadGenerated();
            selfTest(material);
        } catch (Exception failure) {
            if (!isKeyPermanentlyInvalidated(failure)) throw failure;
            if (hasSignerHistory(context)) {
                throw new IOException("the generated Android Keystore signer is permanently invalidated and signer or install history exists; remove affected converted packages before resetting the manager signer", failure);
            }
            // Clearing manager data can leave an unusable Keystore alias behind. With no
            // signer anchor or managed-package records, no recorded install depends on it.
            removeGeneratedKey();
            material = loadGenerated();
            selfTest(material);
        }
        requireAnchoredSigner(context, material.fingerprint());
        return material;
    }

    private static Material loadGenerated() throws Exception {
        KeyStore androidStore = KeyStore.getInstance("AndroidKeyStore");
        androidStore.load(null);
        if (!androidStore.containsAlias(GENERATED_ALIAS)) {
            Calendar start = Calendar.getInstance();
            Calendar end = (Calendar) start.clone();
            end.add(Calendar.YEAR, 25);
            KeyPairGenerator generator = KeyPairGenerator.getInstance("RSA", "AndroidKeyStore");
            generator.initialize(new KeyGenParameterSpec.Builder(GENERATED_ALIAS,
                    KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
                    .setKeySize(3072)
                    .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                    .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
                    .setUserAuthenticationRequired(false)
                    .setCertificateSubject(new javax.security.auth.x500.X500Principal("CN=ZettaBridge Personal Signing"))
                    .setCertificateSerialNumber(java.math.BigInteger.ONE)
                    .setCertificateNotBefore(start.getTime())
                    .setCertificateNotAfter(end.getTime())
                    .build());
            generator.generateKeyPair();
            androidStore.load(null);
        }
        Certificate certificate = androidStore.getCertificate(GENERATED_ALIAS);
        PrivateKey privateKey = (PrivateKey) androidStore.getKey(GENERATED_ALIAS, null);
        if (!(certificate instanceof X509Certificate) || privateKey == null)
            throw new IOException("personal signing key is missing");
        return new Material(privateKey, (X509Certificate) certificate);
    }

    private static void selfTest(Material material) throws Exception {
        byte[] challenge = "ZettaBridge personal signer health check".getBytes(StandardCharsets.UTF_8);
        Signature signer = Signature.getInstance("SHA256withRSA");
        signer.initSign(material.privateKey);
        signer.update(challenge);
        byte[] signed = signer.sign();
        Signature verifier = Signature.getInstance("SHA256withRSA");
        verifier.initVerify(material.certificate.getPublicKey());
        verifier.update(challenge);
        if (!verifier.verify(signed)) throw new IOException("personal signing key failed its certificate check");
    }

    private static boolean isKeyPermanentlyInvalidated(Throwable failure) {
        for (Throwable cause = failure; cause != null; cause = cause.getCause()) {
            if (cause instanceof KeyPermanentlyInvalidatedException) return true;
        }
        return false;
    }

    private static boolean hasSignerHistory(Context context) {
        File records = new File(context.getFilesDir(), "install-records");
        if (new File(records, "personal-signer.sha256").isFile()) return true;
        File[] files = records.listFiles();
        if (files == null) return false;
        for (File file : files) if (file.isFile()) return true;
        return false;
    }

    private static Material loadImported(File directory) throws Exception {
        File aliasFile = new File(directory, "alias");
        File passwordFile = new File(directory, "password.enc");
        if (!aliasFile.isFile() || !passwordFile.isFile()) throw new IOException("imported personal signing key is incomplete");
        String alias = new String(Files.readAllBytes(aliasFile.toPath()), StandardCharsets.UTF_8).trim();
        char[] password = readEncryptedPassword(passwordFile);
        try {
            KeyStore imported = KeyStore.getInstance("PKCS12");
            try (InputStream in = new FileInputStream(new File(directory, "personal.p12"))) {
                imported.load(in, password);
            }
            KeyStore.Entry entry = imported.getEntry(alias, new KeyStore.PasswordProtection(password));
            if (!(entry instanceof KeyStore.PrivateKeyEntry)) throw new IOException("imported signer private key is missing");
            KeyStore.PrivateKeyEntry privateEntry = (KeyStore.PrivateKeyEntry) entry;
            if (!(privateEntry.getCertificate() instanceof X509Certificate)) throw new IOException("imported signer certificate is invalid");
            return new Material(privateEntry.getPrivateKey(), (X509Certificate) privateEntry.getCertificate());
        } finally {
            Arrays.fill(password, '\0');
        }
    }

    private static String findAlias(KeyStore store) throws Exception {
        if (store.containsAlias("zettabridge") && store.isKeyEntry("zettabridge")) return "zettabridge";
        Enumeration<String> aliases = store.aliases();
        String found = null;
        while (aliases.hasMoreElements()) {
            String alias = aliases.nextElement();
            if (!store.isKeyEntry(alias)) continue;
            if (found != null) throw new IOException("PKCS#12 must contain one signing key or the zettabridge alias");
            found = alias;
        }
        if (found == null) throw new IOException("PKCS#12 contains no private key");
        return found;
    }

    private static void requireAnchoredSigner(Context context, String fingerprint) throws IOException {
        File anchor = new File(new File(context.getFilesDir(), "install-records"), "personal-signer.sha256");
        if (!anchor.isFile()) return;
        String existing = new String(readLimited(anchor, 256), StandardCharsets.US_ASCII).trim();
        if (!existing.equalsIgnoreCase(fingerprint))
            throw new IOException("this manager already uses a different personal signer; import the key matching its signer fingerprint");
    }

    private static void writeEncryptedPassword(Context context, File destination, char[] password) throws Exception {
        SecretKey key = passwordKey(true);
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, key);
        byte[] encrypted = cipher.doFinal(new String(password).getBytes(StandardCharsets.UTF_8));
        byte[] iv = cipher.getIV();
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        data.write(iv.length); data.write(iv); data.write(encrypted);
        try (FileOutputStream out = new FileOutputStream(destination)) { out.write(data.toByteArray()); out.flush(); }
    }

    private static char[] readEncryptedPassword(File file) throws Exception {
        byte[] data = readLimited(file, 4096);
        if (data.length < 13) throw new IOException("encrypted key password is truncated");
        int ivLength = data[0] & 255;
        if (ivLength < 12 || ivLength > 16 || data.length <= 1 + ivLength)
            throw new IOException("encrypted key password is malformed");
        byte[] iv = Arrays.copyOfRange(data, 1, 1 + ivLength);
        byte[] encrypted = Arrays.copyOfRange(data, 1 + ivLength, data.length);
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, passwordKey(false), new GCMParameterSpec(128, iv));
        return new String(cipher.doFinal(encrypted), StandardCharsets.UTF_8).toCharArray();
    }

    private static SecretKey passwordKey(boolean create) throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(PASSWORD_KEY_ALIAS)) return (SecretKey) store.getKey(PASSWORD_KEY_ALIAS, null);
        if (!create) throw new IOException("key password encryption key is missing");
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(PASSWORD_KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setUserAuthenticationRequired(false).build());
        return generator.generateKey();
    }

    private static void removeGeneratedKey() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(GENERATED_ALIAS)) store.deleteEntry(GENERATED_ALIAS);
    }

    private static String fingerprint(X509Certificate certificate) throws Exception {
        return hex(MessageDigest.getInstance("SHA-256").digest(certificate.getEncoded()));
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format(Locale.ROOT, "%02x", b & 255));
        return out.toString();
    }

    private static byte[] readLimited(File file, int maxBytes) throws IOException {
        try (InputStream in = new FileInputStream(file); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[1024]; int n;
            while ((n = in.read(buffer)) != -1) {
                if (n > maxBytes - out.size()) throw new IOException("private key metadata exceeds size limit");
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        }
    }
}
