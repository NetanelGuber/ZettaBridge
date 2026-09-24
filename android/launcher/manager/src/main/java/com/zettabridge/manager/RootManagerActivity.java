package com.zettabridge.manager;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;

/** Explicit manager controls. No guest process or native runtime is started here. */
public final class RootManagerActivity extends Activity {
    private static final int PICK_APK = 1;
    private static final int PICK_REPORT = 2;
    private static final int PICK_EXPORT = 3;
    private RootManager manager;
    private ManagedPackage managed;
    private volatile RootManager.StagedApk staged;
    private volatile String report;
    private volatile String pendingExport;
    private RootManager.Cancellation active;
    private TextView status;
    private volatile boolean closed;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        manager = RootManager.kernelSu(new File(getFilesDir(), "root-staging"));
        managed = new ManagedPackage(this);
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        column.setPadding(pad, pad, pad, pad);
        status = new TextView(this);
        try {
            manager.cleanupStaleStaging();
            status.setText("Root is used only for selected PackageManager actions. This separate manager package does not load guest code.");
        } catch (java.io.IOException e) {
            status.setText("Staging cleanup failed: " + e.getMessage());
        }
        column.addView(status);
        addButton(column, "Check KernelSU grant", () -> runOperation(() -> manager.checkGrant(active)));
        addButton(column, "Stage converted base APK", this::pickApk);
        addButton(column, "Select transformation.json", this::pickReport);
        addButton(column, "Review install or same-key update", this::confirmInstall);
        addButton(column, "Remove converted package (delete data)", () -> enterPackage(false));
        addButton(column, "Remove converted package (keep data)", () -> enterPackage(true));
        addButton(column, "Export recovery metadata", this::exportRecord);
        addButton(column, "Cancel active operation", () -> {
            if (active != null) active.cancel();
        });
        ScrollView scroll = new ScrollView(this);
        scroll.addView(column);
        setContentView(scroll);
    }

    private void addButton(LinearLayout column, String label, Runnable action) {
        Button button = new Button(this);
        button.setText(label);
        button.setOnClickListener(v -> action.run());
        column.addView(button, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private void pickApk() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("application/vnd.android.package-archive");
        startActivityForResult(intent, PICK_APK);
    }

    private void pickReport() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE).setType("application/json");
        startActivityForResult(intent, PICK_REPORT);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == PICK_EXPORT) {
            String backup = pendingExport;
            pendingExport = null;
            if (resultCode != RESULT_OK || data == null || data.getData() == null || backup == null) return;
            Uri destination = data.getData();
            start(() -> {
                if (!"content".equals(destination.getScheme())) throw new Exception("invalid backup destination");
                try (OutputStream out = getContentResolver().openOutputStream(destination, "wt")) {
                    if (out == null) throw new Exception("backup destination cannot be opened");
                    out.write(backup.getBytes(StandardCharsets.UTF_8));
                    out.flush();
                }
                return "Exported conversion metadata, hashes, signer and prior version. No APK, app data or private key was exported.";
            });
            return;
        }
        if ((requestCode != PICK_APK && requestCode != PICK_REPORT)
                || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        if (requestCode == PICK_REPORT) {
            start(() -> {
                if (!"content".equals(uri.getScheme())) throw new Exception("select report through DocumentsUI");
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    if (in == null) throw new Exception("report cannot be opened");
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    byte[] buf = new byte[8192];
                    int n;
                    while ((n = in.read(buf)) != -1) {
                        if (bytes.size() + n > 1_000_000) throw new Exception("report exceeds 1 MB");
                        if (active.isCancelled()) throw new Exception("report selection cancelled");
                        bytes.write(buf, 0, n);
                    }
                    report = bytes.toString(StandardCharsets.UTF_8.name());
                    return "Selected transformation report (" + bytes.size() + " bytes). Review install next.";
                }
            });
            return;
        }
        start(() -> {
            RootManager.StagedApk next = manager.stage(getContentResolver(), uri, active);
            if (closed) {
                next.file.delete();
                return "Staging cancelled because the manager closed.";
            }
            RootManager.StagedApk old = staged;
            staged = next;
            if (old != null) old.file.delete();
            return "Staged " + next.size + " bytes\nSHA-256: " + next.sha256;
        });
    }

    private void confirmInstall() {
        RootManager.StagedApk apk = staged;
        String selectedReport = report;
        if (apk == null || selectedReport == null) {
            status.setText("Stage a converted base APK and select its transformation.json first.");
            return;
        }
        start(() -> {
            ManagedPackage.Review review = managed.review(apk, selectedReport);
            runOnUiThread(() -> new AlertDialog.Builder(this)
                    .setTitle(review.update ? "Confirm same-key update" : "Confirm new install")
                    .setMessage(review.summary())
                    .setNegativeButton("Keep current state", null)
                    .setPositiveButton(review.update ? "Update" : "Install", (dialog, which) ->
                            runOperation(() -> managed.install(manager, apk, review, active)))
                    .show());
            return "Package and conversion report verified. Confirm in the dialog.";
        });
    }

    private void enterPackage(boolean keepData) {
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        input.setHint("com.example.app");
        new AlertDialog.Builder(this)
                .setTitle("Package to uninstall")
                .setView(input)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Continue", (dialog, which) -> {
                    String packageName = input.getText().toString().trim();
                    if (!RootManager.validPackage(packageName)) {
                        status.setText("Invalid package name.");
                        return;
                    }
                    new AlertDialog.Builder(this)
                            .setTitle("Confirm uninstall")
                            .setMessage(RootManager.uninstallConsequence(packageName, keepData))
                            .setNegativeButton("Cancel", null)
                            .setPositiveButton("Uninstall", (d, w) -> runOperation(() ->
                                    managed.remove(manager, packageName, keepData,
                                            (action, consequence) -> true, active)))
                            .show();
                }).show();
    }

    private void exportRecord() {
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setHint("com.example.app");
        new AlertDialog.Builder(this)
                .setTitle("Package to back up")
                .setView(input)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Choose destination", (dialog, which) -> {
                    String packageName = input.getText().toString().trim();
                    try {
                        pendingExport = managed.recoveryBackup(packageName);
                        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                                .addCategory(Intent.CATEGORY_OPENABLE)
                                .setType("application/json")
                                .putExtra(Intent.EXTRA_TITLE, "zb-" + packageName + "-recovery.json");
                        startActivityForResult(intent, PICK_EXPORT);
                    } catch (Exception e) {
                        status.setText("Backup failed: " + e.getMessage());
                    }
                }).show();
    }

    private interface Operation { RootManager.Result run(); }

    private void runOperation(Operation operation) {
        start(() -> {
            RootManager.Result result = operation.run();
            return result.state + ": " + result.detail;
        });
    }

    private interface Work { String run() throws Exception; }

    private void start(Work work) {
        if (active != null) {
            status.setText("An operation is already running. Cancel it first.");
            return;
        }
        active = new RootManager.Cancellation();
        status.setText("Working...");
        new Thread(() -> {
            String result;
            try { result = work.run(); }
            catch (Exception e) { result = "FAILED: " + e.getMessage(); }
            String shown = result;
            runOnUiThread(() -> {
                active = null;
                status.setText(shown);
            });
        }, "zb-root-manager").start();
    }

    @Override protected void onDestroy() {
        closed = true;
        if (active != null) active.cancel();
        if (staged != null) staged.file.delete();
        super.onDestroy();
    }
}
