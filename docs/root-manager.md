# Narrow root manager (Step 03)

The separate `com.zettabridge.manager` package owns the **ZettaBridge Manager**
screen and `RootManager`. It has no guest loader, plugin process, or shared UID
with the launcher or any converted app. Its launcher activity is exported only
as an ordinary user-facing entry point. Grant root to this package only.

The manager copies a document-picker `content:` URI into its private staging
directory as its ordinary app UID. It caps the copy at 2 GiB, records SHA-256,
and opens the file with `O_NOFOLLOW` before verifying size and hash. The opened
file descriptor supplies install bytes to PackageManager through stdin. The
selected URI and staging path never enter a root command. Android 17's
`pm install -S BYTES` consumes stdin without a trailing `-` path argument.
Changing the staged file or replacing it with a symlink makes installation fail.
Abandoned staging files are removed when the manager next starts.

The provider seam has four fixed operations: check `id -u`, install without
replacement, install with replacement, and uninstall (with or without the
PackageManager keep-data flag). KernelSU's `su -c` is the initial adapter. Only
a decimal byte count enters an install command; a validated package name for
uninstall is passed as stdin data and expanded as a quoted shell variable in a
constant command. No operation accepts an arbitrary shell command, executable,
guest library, system property, SELinux change, or system partition path.

The UI asks separately before new install, replacement, and either uninstall
mode. New install uses `pm install -R`; replacement uses `pm install -r` and
still obeys Android signer rules. Uninstall without `-k` erases package data;
the alternate option asks PackageManager to retain data. A root grant prompt is
controlled by the provider. Root work is bounded to 120 seconds, can be
cancelled, and reports denial, command failure, or timeout. If cancellation or
timeout occurs after PackageManager starts, the package outcome may be unknown;
inspect package state before retrying.

This step does not convert an APK, sign one, install a split set, back up data,
or run an ARM32 guest. Those workflows belong to later steps. The normal Android
package UID remains the execution boundary for installed apps. The manager
does not pass a root handle to an installed app or to the launcher `:guest`
process; their UIDs differ from the manager's UID.

Focused host checks are in `tests/manager/RootManagerContractsTest.java` and
run with `tests/manager/run_tests.sh` on the documented Linux toolchain. The
test covers consent, valid command set, denial, cancellation, timeout, command
failure, package input isolation, and symlink replacement. The Android build
and device observations are recorded in `plan.md`; they do not establish a
successful on-device root install or converted-app behavior.
