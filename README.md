# ZettaBridge

Run 32-bit (armeabi / armeabi-v7a) Android apps on 64-bit-only ARM phones.

Recent SoCs such as the Snapdragon 8 Elite have no AArch32 execution state at all, so old
32-bit apps simply cannot start on them. ZettaBridge runs such an app inside its own
process: the app's Java code runs on the phone's normal 64-bit ART, and only its native
32-bit code is translated.

**Status: early development, not usable by end users yet.**

## How it works

- **Native code is translated by [Dynarmic](https://github.com/Vita3K/dynarmic)**, an
  ARM dynamic recompiler (0BSD license), from A32/T32 to AArch64.
- **The guest runs the real arm32 Android system libraries.** The bionic linker, libc,
  libm and friends come from an AOSP GSI. ZettaBridge translates Linux syscalls
  (32-bit ABI to 64-bit kernel), not libc functions.
- **A small set of host calls** (OpenGL ES, Android assets, JNI) connects the guest to
  the real device.
- **Nothing is installed.** The launcher imports an APK as a plugin and puts a
  home-screen shortcut for it.

See `CLAUDE.md` for the architecture and decisions, and
`docs/superpowers/specs/2026-09-13-guest-system-boundary-design.md` for the
translator/system boundary.

## What works today

These run on an aarch64 Linux machine with `zbrun`, the command-line runner of the core:
- static and dynamic arm32 Android executables, through the real Android 17 arm32 linker;
- threads, signals, C++ exceptions, `dlopen`;
- loading all native libraries of the first test game (Orange Roulette, a Haxe/OpenFL
  game from 2014).

The core also builds as `libzbridge.so` for arm64 Android. Running it inside an app process
on a phone is the next step (`docs/phase3-device-test.md`).

Not done yet:
- the launcher app;
- the JNI bridge between guest native code and Java;
- OpenGL ES, audio and input passthrough;
- running any actual game.

## Honest limits

- **Speed.** Translated code runs several times slower than native code. A rough
  benchmark on a Snapdragon 8 Elite: integer code about 2x, memory copies about 3.5x,
  and floating-point-heavy loops much more.
- **Every call from the game into the system crosses a translation boundary.**
  **ZettaBridge targets 2D games and utility apps. 3D-heavy games are not a goal.**
- **Apps that need to be really installed are out of scope:** their own UID and
  permissions, visibility to other apps, accounts, push notifications.

## Building (development)

Requirements: aarch64 Linux, clang, CMake, Ninja, Boost headers, and Android NDK r29 for the
guest test programs.

```
git submodule update --init; git -C third_party/dynarmic apply ../patches/dynarmic-0001-thumb32-armv8.patch
```
```
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++; ninja -C build/host
```
```
tools/extract_sysroot.sh; tools/build_guest.sh; ctest --test-dir build/host; tools/run_guest_tests.sh
```

`tools/extract_sysroot.sh` downloads an AOSP GSI (about 1.2 GB) and extracts the arm32
system libraries into `sysroot/`.

## License

Not decided yet. Third-party code keeps its own license; see `third_party/README.md`.
