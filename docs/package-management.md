# Package management (Step 06)

The separate `:manager` app installs and removes converted packages through
Android PackageManager commands under KernelSU. It does not execute guest code
or copy files into `/data/app`. Conversion is an unprivileged host operation;
the output app runs under the UID assigned by Android. This release accepts one
converted base APK and one matching `transformation.json`. It refuses split sets,
custom default processes, shared UIDs, signer changes, downgrades, and any device
with another Android user or work profile. The manager must run as system user 0.
The root user check is a fixed `pm list users` call before every install/remove.
Package code may be shared across Android users, so this restriction prevents a
user-0 action from unexpectedly changing another profile's app code.

## Install and update

1. Build the manager with `cd android/launcher && ./gradlew :manager:assembleDebug`
   in the documented Linux/SDK environment. Convert an ARM32 source with
   `tools/apk_convert.py`; retain its signed `base.apk` and `transformation.json`.
   The converter checks that `libzbridge.so` exports the installed bootstrap JNI
   entry point before publishing. Keep the personal signing key outside the repo.
2. On the owner user, open ZettaBridge Manager. Select **Stage converted base
   APK**, then **Select transformation.json** through DocumentsUI. Use **Review
   install or same-key update**. The review binds the report's APK SHA-256 to the
   staged bytes and compares package, version, signer, default process and ARM64
   host-library layout with PackageManager's archive inspection.
3. Read the package name, version, signer fingerprint and APK hash in the dialog.
   Choose **Install** or **Update** explicitly. A new install uses `pm install -R`;
   an update uses `pm install -r`. Both stream the verified APK to PackageManager
   and target user 0. PackageManager decides install success and the app UID.
4. The manager re-queries the installed package, signer, version, default
   process and Android-assigned UID. It displays the native library directory.
   The converted app appears in system app lists and handles its own runtime
   permission prompts. Device diagnostics can confirm `primaryCpuAbi=arm64-v8a`
   in `dumpsys package`; the manager also rejects non-ARM64 libraries in the host
   namespace before install.

If a package of the same name has another signer, the manager stops before any
root install or removal. **Keep current state** leaves it installed. Renaming is
not offered: a safely renamed copy needs manifest, provider, URI and API identity
rewrites beyond this step. The manager does not silently uninstall the original
or delete its data. The first verified converted install anchors the personal
signer fingerprint in the manager's private storage; later outputs must match it.

## Remove, records and recovery

The two remove controls are separate. **Keep data** asks PackageManager to
uninstall with `-k`; **Delete data** is a second, explicitly confirmed operation.
The manager removes only packages with a successful local converted-install
record whose currently installed signer still matches. It verifies the package
is absent afterward. Neither action touches an unrelated original-signed app.

Private `files/install-records/` stores an atomic JSON record of the conversion
report, input/output hashes, personal signer fingerprint, prior version/UID,
observed UID/process/native-library directory, outcome and one previous record.
An attempt record is written before PackageManager starts, and failed/unknown
attempts do not overwrite the last successful record. `personal-signer.sha256`
anchors the key. **Export recovery metadata** writes the selected package's
record, prior record, latest attempt and signer fingerprint to an owner-selected
JSON document through Android's document picker. It exports no APK, app data or
private key. The manager has `allowBackup=false`; save this exported document,
the signed converted APK/report and the personal signing key separately in
trusted owner-controlled backups.
Records are metadata, not an app-data backup. Package data may contain encrypted,
hardware-bound, certificate-bound or server-bound state that a file copy cannot
restore; `-k` is a PackageManager request, not a recovery guarantee.

Retain the previous signed converted APK and report outside the manager. A failed
PackageManager update should leave the old installation/data in place; if an
operation times out, is interrupted, or post-install verification fails, inspect
system package state before retrying. A successful update cannot generally be
rolled back to a lower version through this manager, because downgrades are
blocked. Produce a repaired same-key APK at the current or a higher version.
If an app is removed with data retained, reinstall only the same-signer package
and verify whether Android retained its data. Do not depend on data restoration
after explicit delete-data removal or signing-key loss.

## Step 06 evidence (2026-09-24)

The converter admission suite passed 4 tests, including a missing installed
bootstrap export. The manager contract test passed owner-only/multi-user checks;
`:manager:assembleDebug` passed with SDK 35. A stale ARM64 bridge lacking
`Java_com_zettabridge_core_ZBridge_activateInstalled` was rejected before
publication, leaving no output directory. The successful converted fixtures
used the previously device-verified bridge from Step 05 and the same personal
signer SHA-256
`d3fe5a914ad2f4139c645ae3a09ba845d484b2f946a50826326beb41d5575c00`.

On Pixel 11 Pro XL `67161FDDV0011Q`, Android 17/API 37 build
`CD1A.260905.001.B1`, KernelSU, sole user 0, the manager installed the
single-APK synthetic fixture as `com.zettabridge.step06managed` with UID 10389,
`primaryCpuAbi=arm64-v8a` and its own default process. The fixture's translated
JNI probe returned `PASS uid=10389 native=12 callbacks=1`. Android displayed
the camera permission prompt; choosing **Don't allow** left
`android.permission.CAMERA: granted=false`. A same-personal-key update from
version 1 to 2 kept UID 10389, a private data marker, the permission denial and
the passing guest probe. The manager's record saved version 2 and prior version
1. Confirmed `-k` removal removed the package while the marker remained; a
fresh same-key install registered it again and preserved the marker. The final
version 2 fixture and manager remain installed for inspection.

A separately signed synthetic package `com.zettabridge.step06fixture` was
installed with UID 10388 and a private marker. Reviewing its converted output
failed with an original-signer conflict before root install; the original
package and marker remained. A mismatched APK/report hash also failed before
root, leaving installed version 2 and its marker intact. These are synthetic
device checks, not support claims for user APKs, split installation, secondary
users, app data backup/restore, or complete Android component lifecycles.

The final export button and Android document-picker write path compiled in the
SDK 35 manager build. The phone disconnected before this final UI path could be
exercised on-device; install/update/conflict/remove behavior above was tested
on-device.

Platform references: [PackageInfo signing/version fields](https://developer.android.com/reference/android/content/pm/PackageInfo),
[PackageInstaller session semantics](https://developer.android.com/reference/android/content/pm/PackageInstaller),
[Android app signing](https://developer.android.com/studio/publish/app-signing).
