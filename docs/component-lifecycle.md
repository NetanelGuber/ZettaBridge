# Android components and lifecycle (Step 09)

## Contract

Conversion keeps the source manifest tree, including activity aliases, task and
launch attributes, orientation and configuration flags, themes, metadata,
permissions, intent filters, provider authorities and process names. The only
manifest edits are `extractNativeLibs=true` and one private, highest-order
bootstrap provider for each ordinary declared process (up to eight). Output
verification compares the complete parsed manifest tree after removing those
recorded edits, and separately checks retained DEX/resource bytes. Android's
PackageManager, ActivityManager, ContentResolver, AlarmManager and
NotificationManager continue to own package and component behavior. Converted
packages do not use the legacy launcher PackageManager hook.

The bootstrap provider activates the guest runtime before source providers and
`Application.onCreate`, and therefore before ordinary Activity, Service and
Receiver callbacks. Android constructs a custom Application and calls its
class initializer/`attachBaseContext` before providers. A native load in either
early hook is outside this bootstrap contract; preflight emits
`early_application_load` for every custom Application so it is reviewed rather
than claimed as proven. A source custom component factory or dynamic feature
startup also needs separate app-specific validation. The fixture uses a custom
Application with no early native load, then proves native JNI in its `onCreate`.

Preflight rejects isolated/external services, `hasCode=false`, direct-boot
components, multiprocess providers, more than eight declared app processes and
generated bootstrap authority collisions. These cannot safely use the current
credential-protected, per-process bundle. NativeActivity remains Step 10 work.
The converter still rejects shared UID and unsupported native entry points.
The injected providers are unexported and do not grant URI or package access.

## Reproduce

In the pinned WSL/NDK environment from `docs/development.md`, build the guest
library and Android ARM64 runtime/bundle, then run:

```sh
sh tools/make_step09_fixture.sh
cd android/launcher
ANDROID_HOME="$ANDROID_SDK_ROOT" ./gradlew :step05bootstrap:assembleDebug :step09fixture:assembleDebug
cd ../..
python3 tools/apk_convert.py convert \
  android/launcher/step09fixture/build/outputs/apk/debug/step09fixture-debug.apk \
  --output-dir build/step09-converted \
  --apksigner "$ANDROID_SDK_ROOT/build-tools/35.0.0/apksigner" \
  --zipalign "$ANDROID_SDK_ROOT/build-tools/35.0.0/zipalign"
```

The source fixture contains only its Java components and the ARM32 probe ELF.
The exact device run below reused the locally device-tested Step 08 runtime
assets, bridge and proxy from `build/step08-converted3/base.apk`; no third-party
APK or root-executed guest code was used. Rebuilding the runtime from source is
the normal reproducible path above. The converter's personal signing key remains
outside the repository.

## Step 09 evidence (2026-09-26)

- Windows Python 3.12: 11 preflight and 7 converter tests passed. The tests
  cover unsupported provider/startup modes, process and authority limits, and
  detection of nested manifest changes. `git diff --check` passed. The Gradle
  `:step09fixture:assembleDebug` and `:step05bootstrap:assembleDebug` builds
  passed with SDK 35. The current converter signed and structurally verified
  the output, including exact source component/filter/metadata preservation.
- Source APK SHA-256:
  `89f49a1ee9b220bb1e6d0015d7c2c35085061df258bfdb6d0fab213359b1c856`.
  Converted APK SHA-256:
  `7369ccb7b5c142bbcb1ad70f0f6230bb7384b6bf3d9759336d2b06544f0c4aba`.
  Output signer SHA-256:
  `d3fe5a914ad2f4139c645ae3a09ba845d484b2f946a50826326beb41d5575c00`.
  The final `build/step09-converted5/base.apk` has the same bytes as the
  previously exercised `converted3` APK. Both are ignored local artifacts.
- Pixel 11 Pro XL, Android 17/API 37, enforcing SELinux, serial
  `67161FDDV0011Q`: `adb install -r` succeeded. Fresh event logs from the
  converted package show `application-attach`, guest-JNI source provider,
  guest-JNI `Application.onCreate`, then Activity. The `:worker` process shows
  its own attach, guest-JNI provider and Application, followed by content query,
  Service and alarm Receiver. Main PID 21071 and worker PID 21354 used UID
  10391. After force-stop, a fresh launch restarted both processes with new
  PIDs while retaining UID 10391; source events were observed again.
- The Activity queried its own installed package, alias, metadata, provider and
  resource package through Android APIs. `adb shell content query/read` returned
  `com.zettabridge.step09fixture` and `step09-file-provider`. The deep link
  arrived through Android intent resolution; a repeated launch delivered
  `onNewIntent` for `singleTop`. Recents showed the source alias as the original
  activity, the real Activity as its target, and the declared task affinity.
  The manifest's orientation, theme, permissions and nested intent/filter data
  were preserved by exact tree comparison; no claim about visual theme quality
  is made from that static check.
- With the phone awake and its dream overlay temporarily disabled, `wm density`
  caused Android to recreate `ConfigActivity`. A `wm size` change produced the
  declared `onConfigurationChanged` callback. Physical display overrides and
  the prior dream setting were restored. This is synthetic lifecycle evidence.
- AlarmManager delivered a PendingIntent to the `:worker` Receiver. With
  `POST_NOTIFICATIONS` granted, NotificationManager held the fixture
  notification under the converted package and UID. After permission revocation,
  an explicit Receiver dispatch logged `notification-permission-denied`; the
  permission was restored. No permission or other-package identity was forged.

These results cover one synthetic APK on one device. They do not establish
general user-app compatibility, early custom-Application native loading,
dynamic feature behavior, or later NDK/graphics/media steps.

Android's documented provider `initOrder` and component factory timing inform
the startup boundary: [provider manifest](https://developer.android.com/guide/topics/manifest/provider-element),
[AppComponentFactory](https://developer.android.com/reference/android/app/AppComponentFactory).
