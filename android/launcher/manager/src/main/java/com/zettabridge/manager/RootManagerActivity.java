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

/** Explicit manager controls. No guest process or native runtime is started here. */
public final class RootManagerActivity extends Activity {
    private static final int PICK_APK = 1;
    private RootManager manager;
    private volatile RootManager.StagedApk staged;
    private RootManager.Cancellation active;
    private TextView status;
    private volatile boolean closed;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        manager = RootManager.kernelSu(new File(getFilesDir(), "root-staging"));
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
        addButton(column, "Stage an APK", this::pickApk);
        addButton(column, "Install new package", () -> confirmInstall(false));
        addButton(column, "Replace installed package", () -> confirmInstall(true));
        addButton(column, "Uninstall package", () -> enterPackage(false));
        addButton(column, "Uninstall and keep data", () -> enterPackage(true));
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

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_APK || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
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

    private void confirmInstall(boolean replace) {
        RootManager.StagedApk apk = staged;
        if (apk == null) {
            status.setText("Select and stage an APK first.");
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(replace ? "Confirm replacement" : "Confirm installation")
                .setMessage(RootManager.installConsequence(apk, replace))
                .setNegativeButton("Cancel", null)
                .setPositiveButton(replace ? "Replace" : "Install", (dialog, which) ->
                        runOperation(() -> manager.install(apk, replace, (action, consequence) -> true, active)))
                .show();
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
                                    manager.uninstall(packageName, keepData,
                                            (action, consequence) -> true, active)))
                            .show();
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
