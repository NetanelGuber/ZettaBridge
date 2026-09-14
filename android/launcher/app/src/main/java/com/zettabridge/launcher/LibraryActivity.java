package com.zettabridge.launcher;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.Typeface;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Library screen (main process): import APKs, launch them, pin shortcuts, delete. UI built in code. */
public class LibraryActivity extends Activity {
    private static final String TAG = "zb-launcher";
    private static final int REQUEST_IMPORT = 1;

    private final List<PluginRecord> plugins = new ArrayList<>();
    private BaseAdapter adapter;
    private TextView emptyView;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        root.setFitsSystemWindows(true);

        Button importButton = new Button(this);
        importButton.setText("Import APK");
        importButton.setOnClickListener(v -> pickApk());
        root.addView(importButton, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        emptyView = new TextView(this);
        emptyView.setText("No apps yet. Import an APK to add one. Tap to launch, long-press for more.");
        emptyView.setPadding(0, pad, 0, pad);
        root.addView(emptyView);

        ListView list = new ListView(this);
        adapter = new BaseAdapter() {
            @Override
            public int getCount() {
                return plugins.size();
            }

            @Override
            public Object getItem(int position) {
                return plugins.get(position);
            }

            @Override
            public long getItemId(int position) {
                return position;
            }

            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                return row(plugins.get(position));
            }
        };
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> launch(plugins.get(position)));
        list.setOnItemLongClickListener((parent, view, position, id) -> {
            showActions(plugins.get(position));
            return true;
        });
        root.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void refresh() {
        plugins.clear();
        plugins.addAll(PluginStore.list(this));
        emptyView.setVisibility(plugins.isEmpty() ? View.VISIBLE : View.GONE);
        adapter.notifyDataSetChanged();
    }

    private View row(PluginRecord r) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(8), 0, dp(8));

        ImageView icon = new ImageView(this);
        Bitmap bmp = r.icon();
        if (bmp != null) {
            icon.setImageBitmap(bmp);
        } else {
            icon.setImageResource(android.R.drawable.sym_def_app_icon);
        }
        row.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.setPadding(dp(12), 0, 0, 0);
        TextView title = new TextView(this);
        title.setText(r.label);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        TextView details = new TextView(this);
        details.setText(r.packageName + (r.versionName != null ? " " + r.versionName : "") + "\n" + r.status());
        texts.addView(title);
        texts.addView(details);
        row.addView(texts);
        return row;
    }

    private void pickApk() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*")
                .putExtra(Intent.EXTRA_MIME_TYPES,
                        new String[] {"application/vnd.android.package-archive", "application/octet-stream"});
        startActivityForResult(intent, REQUEST_IMPORT);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_IMPORT && resultCode == RESULT_OK && data != null && data.getData() != null) {
            importApk(data.getData());
        }
    }

    private void importApk(Uri uri) {
        // A running plugin keeps the old APK mapped: stop the guest process first.
        stopGuestProcess();
        AlertDialog progress = new AlertDialog.Builder(this).setMessage("Importing...").setCancelable(false).show();
        new Thread(() -> {
            String message;
            try {
                PluginRecord r = PluginStore.importApk(this, uri);
                message = "Imported " + r.label + " (" + r.status() + ")";
            } catch (Exception e) {
                Log.e(TAG, "import failed", e);
                message = "Import failed: " + e.getMessage();
            }
            String shown = message;
            runOnUiThread(() -> {
                progress.dismiss();
                Toast.makeText(this, shown, Toast.LENGTH_LONG).show();
                refresh();
            });
        }, "zb-import").start();
    }

    private void launch(PluginRecord r) {
        if (!r.isLaunchable()) {
            Toast.makeText(this, r.label + ": " + r.status(), Toast.LENGTH_LONG).show();
            return;
        }
        startActivity(GuestLaunchActivity.intent(this, r.packageName));
    }

    private void showActions(PluginRecord r) {
        new AlertDialog.Builder(this)
                .setTitle(r.label)
                .setItems(new String[] {"Launch", "Pin shortcut", "Delete"}, (dialog, which) -> {
                    if (which == 0) launch(r);
                    if (which == 1) pinShortcut(r);
                    if (which == 2) confirmDelete(r);
                })
                .show();
    }

    private static String shortcutId(PluginRecord r) {
        return "plugin:" + r.packageName;
    }

    private void pinShortcut(PluginRecord r) {
        ShortcutManager sm = getSystemService(ShortcutManager.class);
        if (sm == null || !sm.isRequestPinShortcutSupported()) {
            Toast.makeText(this, "The home screen does not support pinned shortcuts", Toast.LENGTH_LONG).show();
            return;
        }
        Bitmap bmp = r.icon();
        Icon icon = bmp != null ? Icon.createWithBitmap(bmp)
                : Icon.createWithResource(this, android.R.drawable.sym_def_app_icon);
        ShortcutInfo info = new ShortcutInfo.Builder(this, shortcutId(r))
                .setShortLabel(r.label)
                .setLongLabel(r.label)
                .setIcon(icon)
                .setIntent(GuestLaunchActivity.intent(this, r.packageName))
                .build();
        sm.requestPinShortcut(info, null);
    }

    private void confirmDelete(PluginRecord r) {
        new AlertDialog.Builder(this)
                .setMessage("Delete " + r.label + " and its data?")
                .setPositiveButton("Delete", (dialog, which) -> {
                    stopGuestProcess();
                    ShortcutManager sm = getSystemService(ShortcutManager.class);
                    if (sm != null) {
                        try {
                            sm.disableShortcuts(Collections.singletonList(shortcutId(r)), "App was deleted");
                        } catch (RuntimeException e) {
                            Log.w(TAG, "cannot disable shortcut: " + e);
                        }
                    }
                    PluginStore.delete(r);
                    refresh();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    /** Kills our own :guest process, if running (allowed for processes of the same uid). */
    private void stopGuestProcess() {
        ActivityManager am = getSystemService(ActivityManager.class);
        if (am == null || am.getRunningAppProcesses() == null) return;
        for (ActivityManager.RunningAppProcessInfo proc : am.getRunningAppProcesses()) {
            if (proc.processName.endsWith(ZbApplication.GUEST_SUFFIX)) android.os.Process.killProcess(proc.pid);
        }
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics());
    }
}
