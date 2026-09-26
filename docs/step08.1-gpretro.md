# Step 08.1: GP Retro JNI startup

## Failure and fix

The same `GPRetro.apk` used in the 2026-09-26 device attempt has SHA-256
`78b231c17d8d7ff8f0d2c66321a7121fe8d5bcc116966dc83963c6150f4a7010`.
It contains `lib/armeabi/libgame.so` with `DT_TEXTREL` and no `DF_TEXTREL`.
The first converted diagnostic APK preserved the source target SDK 13. Android
17 required `adb install --bypass-low-target-sdk-block`; no SDK field was raised.
On Pixel 11 Pro XL `67161FDDV0011Q`, its launcher Activity failed at
`GP.<clinit>` with ART's generic `JNI_ERR returned from JNI_OnLoad`.

The new per-process report, created before runtime installation and proxy
loading, gave the underlying failure in `zb-runtime-30189.txt`:

```
proxy-failed: libgame.so kind=relocation_error guest dlopen failed: dlopen failed: "/data/user/0/com.smallthinggame.gpretro/files/zb/app/lib/libgame.so" has text relocations
jni-onload-calls: 0
```

The installed bootstrap had copied the ARM32 library unchanged. The plugin
import path already ran `ZBridge.fixGuestLibrary`, which rewrites `DT_TEXTREL`
to the private `DT_ZB_TEXTREL` marker for the guest loader. The installed path
now runs that same checked fixup on each extracted `zb/app/lib/*.so` before
making it read-only or publishing the runtime directory. It rejects skipped or
malformed libraries. The extraction marker now includes `elf-fixups-v1`, so an
update re-extracts earlier unprepared copies even when asset bytes are the same.

The bootstrap writes one report per PID under the package's external files
directory, with an internal-files fallback. It leaves an earlier report intact
after a crash or restart. On this device, retrieve a report with:

```sh
adb shell ls /sdcard/Android/data/com.smallthinggame.gpretro/files/zb-reports
adb pull /sdcard/Android/data/com.smallthinggame.gpretro/files/zb-reports/zb-runtime-30189.txt
```

If external files are unavailable, the fallback is
`/data/user/0/com.smallthinggame.gpretro/files/zb-reports`, retrievable with
the installed package UID or root access.

## Validation

The fixed output is `build/step081-fixed1/base.apk`, SHA-256
`aaaa458fb021649dead1db192f28d2f70febe707f684e897828947193551f990`.
`build/step081-fixed1/transformation.json` records the source and output hashes,
ARM32 library mapping, and the added bootstrap providers. `aapt dump badging`
still reports `sdkVersion:'9'` and `targetSdkVersion:'13'`.

On the same Pixel/API 37, the first corrected process (PID 30609) reported one
successful `libgame.so` proxy load, `JNI_OnLoad` returning `0x00010002`, 48
registered natives, and no proxy failures. The game rendered its main menu;
tapping Play opened the next selection screen. After `am force-stop`, a fresh
process (PID 31081) again reported one successful `JNI_OnLoad`, zero proxy
failures, and rendered the menu. The old failed report (PID 30189) and first
successful report remained retrievable after this restart. Local evidence is in
`build/step081-report-launch1.txt`, `build/step081-report-launch2.txt`,
`build/step081-menu.png`, `build/step081-play.png`, and
`build/step081-fresh-menu.png` (ignored build artifacts).

`elf_fixups_test` now includes a synthetic ARM ELF32 fixture with `DT_TEXTREL`
alone, the shape found in GP Retro. A second generated ARM32 JNI fixture has
real text relocations: AArch64/QEMU `jni_loader_test` confirms the raw library
fails at guest `dlopen` and its prepared copy reaches `JNI_OnLoad`, returns
JNI 1.6, and binds four natives. AArch64/QEMU `elf_fixups_test`,
`jni_loader_test`, `proxy_runtime_test`, `jni_bridge_test`, and both
`guest_jni_engine_test` modes passed. `tools/gen_jni.py --check` passed with
NDK r29 (`NDK_HOST=linux-x86_64`); the ARM32 guest and bootstrap Gradle builds
passed.

The original creator's build was not present for a controlled binary/device
comparison. Play Games authorization returned `DEVELOPER_ERROR` for the newly
signed package; sign-in was canceled and the local menu still worked. No
gameplay, account service, split APK, other app, or general compatibility claim
is made from this test.

## Manager converter follow-on

The manager build task packages `step05bootstrap-debug.apk` as
`assets/converter/bootstrap.apk`. The manager APK built after this fix was
installed as an update on the same Pixel, preserving its app data. The embedded
bootstrap SHA-256 matched the Step 08.1 bootstrap build:
`d94d4b86332a952f25c3bd4d654f519c5a007b0152b76aade6fc10fd29fc70b2`.

The Manager converted the same source APK through its on-device picker and
reached its install review after signing and output verification. The generated
APK SHA-256 was
`bfc3352456e577e80e428c0649925c6b8942d6139b38dfcb6caee0590488bbd6`.
The converted DEX contains `elf-fixups-v1` and `fixGuestLibrary`; its ARM64
`libzbridge.so` matched the built bridge SHA-256
`925a37dad6f82f7c4fba190b6dad9cfac03ebc5687ed2cc36eab00868031d470`.
SDK `apksigner` verified its v2/v3 signatures. This in-app path currently raises
the source's SDK 9/13 to min/target 26/26 as a separate install policy; that
SDK change is not counted as the JNI fix. The install was canceled because the
device already had the separately signed CLI-converted copy. No Manager-output
startup result is claimed. The earlier Manager signing failure did not recur,
but its original cause is unknown.
