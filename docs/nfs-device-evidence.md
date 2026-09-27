# NFS Most Wanted device evidence, 2026-09-26

## Google Play extraction: active device run

The user connected an old Xiaomi 22071212AG (Android API 35) with the game
installed by `com.android.vending`. `pm path` showed one base APK and no splits.
Only that APK and its matching OBB were pulled. No save, app-private data, or
account file was transferred. The old phone's progress was untouched. The
Play APK and OBB are a different pair from the EasyAPK files documented below.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| Play base APK | 19,445,609 | `de219abfec8d98aec29fb7f0cc3c22de360a87ce543487a940b0db998912df5f` |
| Play main OBB | 623,470,192 | `66dd4e695e698929f789e7c825eabe3ba5a50ed2ce28b628c96e5dbc008043a1` |
| First converted APK | see local report | `a6a759b959f52261e1cdd63cb6d519ed02191e9c1bc34bb5edd227eb1c14f271` |
| Converted APK with CP15 timer fix | see local report | `8d230e277aa206e14e41b9eebc320714d259700b774dc105970bab0509256301` |

Phone and local source hashes matched. `unzip -t` passed on the OBB. The shared
`published/fonts/gothamblack.ttf` asset matched byte for byte. The Play APK's
package is `com.ea.games.nfs13_row`, version `1003128`/`1.3.128`, min SDK 16,
target SDK 28. Its source signer is
`5bff7d614e1ba11a566abb589c863b013f79aa747bd2146733366c5625a5f0d2`.
It contains five ARM32 and five matching x86 libraries, with no ARM64 ones.
Preflight with both the sysroot and guest library roots returned `analyzed`,
zero missing guest libraries, and zero unresolved strong symbols.

The converter was extended to select the ARM32 libraries and discard matching
x86 variants, while rejecting x86-only library names. Eight converter unit
tests passed. The converted APK was zip-aligned, verified by `apksigner` for
v1/v2/v3, and reported as `arm64-v8a` by `aapt2`. Its manifest changes are
`extractNativeLibs=true` and the private bootstrap provider; original DEX,
resources, package/version and target SDK were preserved. The converted signer
is the existing personal certificate
`d3fe5a914ad2f4139c645ae3a09ba845d484b2f946a50826326beb41d5575c00`.

On Pixel 11 Pro XL `67161FDDV0011Q`, build
`google/kodiak/kodiak:17/CD1A.260905.001.B1/16238327`, the old failed test
installation was removed before the Play conversion was installed. This gave
the game a fresh Pixel app state; PackageManager assigned UID `10402`, with
`primaryCpuAbi=arm64-v8a` and `extractNativeLibs=true`. The exact Play OBB was
placed in the normal `/sdcard/Android/obb/com.ea.games.nfs13_row/` directory;
the Pixel copy retained its SHA-256. At 19:26, `ObbActivity` reported the
expected 623,470,192 bytes and `EXPANSION FILE DELIVERED!`.

The first Play-derived launch got past bootstrap and began `libapp.so` loading,
then aborted in Dynarmic's unsupported `A32CoprocGetTwoWords` emitter. Static
disassembly of `libapp.so` identified a real `MRRC p15, 1, r0, r1, c14` timer
read. Arm's ARMv7 reference identifies that encoding as the virtual count
register CNTVCT. ZettaBridge's CP15 previously handled only thread ID registers.
The generic fix now exposes a coherent monotonic nanosecond CNTVCT and 1 GHz
CNTFRQ; the focused `cp15_test` passed under AArch64 QEMU and the Android ARM64
bridge rebuilt. The existing dirty Dynarmic checkout was not edited.

With that bridge, Pixel launch PID `18441` at 19:33 passed `libapp.so`
`JNI_OnLoad` (32 natives bound), recognized the OBB, and rendered the splash
screen. A first-run EA agreement screen was captured next. The runtime report
shows five proxy loads, zero proxy failures, two successful `JNI_OnLoad` calls,
37 registered natives, zero unimplemented host calls, and active GLES plus
FMOD/AudioTrack JNI traffic. `egl-swaps: 0` is the bridge's counter and does
not negate the observed rendered screenshots. Logcat shows `onResult(GOOGL_DRM,-1)`
followed by `STATE_GAME_START`; this establishes continued startup, not a
general license verdict. It also logs an unimplemented `sched_setscheduler`
call; no resulting gameplay failure has been established. The user reviewed
the agreement and started a fresh game. `pixel-current.png` captures a live
race with car, textured road, signage, HUD, time `1:27.05`, position `1/6`,
speed `147`, and a drift counter. The game was still running as PID `18441`.
The report recorded 3,745 `nativeTouchScreenEvent` calls, 25,663 run-loop
ticks, over 5,000 calls each to FMOD `fmodGetInfo` and `fmodProcess`, zero
proxy failures, zero unimplemented host calls, and no guest exit. This is
real-device visual and input-path evidence. A later screenshot,
`pixel-current2.png`, shows `WELCOME TO FAIRHAVEN COMPLETE`, `PLACED 1ST`,
and the reward screen: the first local race completed without a bridge crash.
The user confirmed audible game sound and responsive steering/touch on this
Pixel run. At 19:41:51, while swiping across locked cars, the original
normal-speed process PID `18441` hit a guest `SIGSEGV`: write to address
`0x00000030`, with the reported block start in `libapp.so` at offset `0x565b78`
and LR at offset `0x473a5c`. The report says `crash-precise: no`, so that PC
is not the exact faulting instruction. The crash happened immediately after
car model/texture loading log entries, including a Mitsubishi Lancer Evolution X
model. There was no recorded missing guest symbol, proxy failure, JNI load
failure, or GL error. A null guest pointer or a timing-sensitive resource path
is plausible, but the cause is not yet established. Artifacts include
`pixel-runtime-18441-crash.txt` and `pixel-crash-logcat.log`.

A temporary diagnostic build set Dynarmic's existing precise memory-fault
mode on for all guest threads. It was installed with `adb install -r` and did
not clear the save. The user swiped across locked cars without a crash, though
car loading felt slightly slower. PID `19726` reported no guest exit or GL
error; `pixel-runtime-19726-precise.txt` and `pixel-precise.png` capture that
run. The temporary source change was removed, the normal ARM64 bridge was
rebuilt, and the normal-speed APK was reinstalled with `-r`. Reproduction on
that build stayed stable while the user browsed several locked cars, including
the Mitsubishi Lancer Evolution X. Twelve rapid right/left car-browser tap
pairs also left the process running. Android Home then reopened the game in
the same PID `20342`; logcat recorded `onRestart`, `onStart`, `onResume`, and
rendering. A force-stop and fresh launcher start created PID `21163`, whose
report showed five proxy loads, zero proxy failures, successful `libapp.so`
`JNI_OnLoad`, and no guest exit at capture time. The saved game survived both
package updates and the cold process launch. The original one-time locked-car
crash remains an open, non-reproduced limitation; no speculative game or
resource workaround was applied.

Local-only artifacts: `C:\Users\Netanel\Documents\Codex\nfs-compat-20260926\play-phone`
contains original APK/OBB, preflight, both conversion reports and outputs,
`pixel-runtime-17853.txt`, `pixel-runtime-18441-gameplay.txt`,
`pixel-gameplay-filtered.log`, `pixel-timer.png` (splash), `pixel-timer2.png`
(agreement), `pixel-current.png` (race), and `pixel-current2.png` (result).
Commercial files stay out of Git.

## Earlier EasyAPK attempt (separate pair)

This records the first implementation attempt for [NFS_COMPATIBILITY.md](../NFS_COMPATIBILITY.md).
It used the user's version `1003128` EasyAPK/Telegram APK and matching OBB.
That source was not a verified Google Play extraction. For that pair, the last
passing gate was conversion (Gate 1); its real-game launch ended before the
ZettaBridge runtime started.

## Gate 0: pair and startup review

- Target: Pixel 11 Pro XL `67161FDDV0011Q`, Android 17/API 37, build
  `google/kodiak/kodiak:17/CD1A.260905.001.B1/16238327`.
- Device APK SHA-256: `a1ec79d5427321870d20c844ba61356cf7ca684ec5ef40e3a564be0c55327c5e`.
  Device OBB SHA-256: `258ca6b6983920e026cdba7bc26ee35c0bdf831b6e1d44a599699534de194a9a`.
  These match the handoff. Device `unzip -t` passed again.
- Before installation, `pm path com.ea.games.nfs13_row` returned no package;
  `/sdcard/Android/obb/com.ea.games.nfs13_row` did not exist. No original
  installed package or its app data was replaced.
- Static DEX inspection: the custom `Application` wrapper
  `com.ea.games.nfs13.GameActivity.attachBaseContext` constructs `a/a/a/e/a`
  (package/configuration checks) and calls `a/a/a/d/a.k` (file/class-loader
  setup) before any provider. Its `onCreate` reflectively constructs another
  `Application`. The wrapper can therefore stop startup before bootstrap.
- `GameActivityMain.getObbFullPath` uses `getObbDir()` and the expansion
  helper's main-OBB filename for the installed version. This supports the
  selected normal OBB location. Runtime recognition was not reached.

## Gate 1: conversion passed

Source revision: `af46df9` on `main`; the existing modified
`third_party/dynarmic` checkout was preserved. Read-only
`tools/apk_preflight.py --sysroot sysroot --guest-lib-dir build/guest/lib`
returned `analyzed` for five `armeabi-v7a` libraries, with zero missing
guest libraries and zero unresolved strong symbols. It retained the
early-custom-Application warning. This is static linkage evidence.

`tools/apk_convert.py` signed and verified one ARM64-installable base APK.
`transformation.json` records the original hash above and output SHA-256
`b4ea12dc87cf77e2b1d97f8c975ebe79f827129136d2e6de666fd96ba97314bc`.
It maps `libNimble.so`, `libapp.so`, `libc++_shared.so`, `libfmodevent.so`, and
`libfmodex.so` into guest assets with matching ARM64 proxies. Manifest edits
are `extractNativeLibs=true` and one private bootstrap provider. It records
no resource edits. Original DEX, resources, package/version, target SDK 28,
and optional wallpaper APK were preserved by output verification. No ARM32
library remains host-loadable.

The source certificate SHA-256 is
`1ccecacceb9068e7ac83c88f088069a8019cb25a07bb4b6a5f69afdceff53eb1`;
the output uses the existing personal signer
`d3fe5a914ad2f4139c645ae3a09ba845d484b2f946a50826326beb41d5575c00`.
`zipalign -c 4`, `apksigner verify` (v1/v2/v3), and `aapt2 dump badging`
passed. Badging reports `arm64-v8a`, unchanged package/version and target
SDK. These are conversion checks, not startup evidence.

## Pixel installation and first runtime boundary

Plain `adb install` succeeded. PackageManager assigned UID `10401` and
reported `primaryCpuAbi=arm64-v8a`, `extractNativeLibs=true`, and version
`1003128`. The validated OBB was copied unchanged to
`/sdcard/Android/obb/com.ea.games.nfs13_row/main.1003128.com.ea.games.nfs13_row.obb`;
the placed file's SHA-256 matched the source OBB. No storage permission or
OBB recognition result was observed because startup ended first.

Launch PID `15910`, followed by PID `16105` after `am force-stop`, ended
before an Activity was displayed. Both runs had this sequence (second run):

```text
19:07:08.346 Start proc 16105:com.ea.games.nfs13_row/u0a401
19:07:08.377 NotificationService: No Channel found ... channelId=ApkProtectorHighImportance ... callingUid=10401
19:07:08.380 games.nfs13_row: System.exit called, status: 0
19:07:08.380 AndroidRuntime: VM exiting with result code 0
```

Static DEX inspection shows the early protector's `a/a/a/f/a.e` posts a
notification and calls `a/a/a/f/a.c`, which reflectively invokes a process
exit. The exact failed protector check is unconfirmed. The changed signer
and third-party APK provenance are possible factors, not proven causes.
The bridge's `zb-reports` directory was absent before and after both launches;
no proxy or guest `libapp.so` load, `JNI_OnLoad`, license decision, OBB lookup,
graphics, or race was reached. Thus Gates 2, 3, and 4 are incomplete.
This is an observed app-protection startup boundary, not a demonstrated
GLES, JNI, loader, or OBB failure. Do not change the protector or infer how
an official Play extraction would behave.

Local-only artifacts are under
`C:\Users\Netanel\Documents\Codex\nfs-compat-20260926`:
`NFS-original.apk`, `preflight.json`, `converted/transformation.json`,
`converted/base.apk`, `device-startup-filtered.log`, and `launch1.png`
(home screen after exit). These APK and game artifacts must remain out of Git.
No broader `plan.md` step was marked complete from this attempt.
