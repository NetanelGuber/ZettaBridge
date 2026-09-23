# Build and developer workflow

`plan.md` is the canonical project handoff. This document contains repeatable
checkout, dependency, and baseline build commands. Historical implementation plans
under `docs/superpowers/` may mention the retired root `CLAUDE.md` or `AGENTS.md`;
those references are archival and are not build dependencies.

## Provision the build tools

On a Debian/Ubuntu ARM64 host, install the native build and extraction tools:

```sh
sudo apt-get update
sudo apt-get install git clang ninja-build openjdk-21-jdk python3 libboost-dev curl unzip e2fsprogs
```

Install Android SDK command-line tools, then install the versions used by this
repository:

```sh
export ANDROID_SDK_ROOT="$HOME/android-sdk"
"$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager" \
  "cmake;3.22.1" "ndk;29.0.14206865" "platforms;android-35" "build-tools;35.0.0"
```

Accept the Android SDK license prompts. Review and accept the GSI terms on the
official release page before using it; the script downloads the pinned direct URL
and cannot display or record page acceptance.

## Repository and toolchain

- Keep the working branch `main` tracking the personal fork's `origin/main`.
  `upstream` is the original project (`ZailoxTT/ZettaBridge`); never push there.
- The reproducible native baseline is Linux ARM64 with Clang/Clang++, CMake 3.22.1,
  Ninja, Python 3, Boost headers, `curl`, `unzip`, `debugfs`, `sha256sum`, and `df`.
- Use Android NDK `29.0.14206865` with the `linux-arm64` prebuilt toolchain. Set
  `NDK` and `NDK_HOST` explicitly so guest builds and header-based generators use
  the same pinned NDK.
- The launcher uses Java 21, Gradle wrapper 8.11.1, Android Gradle Plugin 8.7.3,
  Android SDK platform 35, and Android build-tools 35.0.0. The core Android link
  uses NDK r29 and platform 29.
- Use the system Boost development package for host builds. The Android CMake
  configure needs a directory containing only a `boost/` header link; the command
  below creates it under ignored `build/`.

## Clean checkout and local Dynarmic patches

```sh
git clone https://github.com/NetanelGuber/ZettaBridge.git
cd ZettaBridge
git remote add upstream https://github.com/ZailoxTT/ZettaBridge.git
git submodule update --init --recursive
tools/prepare_dynarmic.sh
```

`tools/prepare_dynarmic.sh` checks out the exact gitlink SHA recorded by this
repository, applies both required patches in order, and recognizes patches that
are already present. It exits if the submodule revision is wrong or a patch is
partially applied/incompatible. It does not change or stage the Dynarmic gitlink.
The patch changes intentionally remain local in the submodule worktree; do not
stage `third_party/dynarmic` when committing project work.

## Guest sysroot

```sh
export NDK="$HOME/android-ndk-r29"
export NDK_HOST=linux-arm64
tools/extract_sysroot.sh
```

The extractor uses the pinned Android 17 QPR 2 AOSP ARM64 GSI from Google's
[official GSI release page](https://developer.android.com/about/versions/17/qpr2/gsi-release-notes).
It checks the published SHA-256 and archive/image sizes, resumes an interrupted
archive download, and writes each extracted file atomically with a local checksum
sidecar. A rerun reuses verified files and restarts only an incomplete unit. The
archive, image, and generated ARM32 sysroot are local inputs, not repository
artifacts. Google says GSIs are for app validation and must not be redistributed
except as permitted by the terms included with the individual download; retain and
follow those terms for any derived files. `sysroot/` is ignored by Git.

The cache is `.cache/gsi/`; it can contain the 1,173,930,919-byte archive and the
uncompressed system image. The extractor checks free space before each large write
and refuses an image larger than 16 GiB. Remove cached files manually after a
successful extraction if disk space is needed; the verified files allow later
reruns without another download.

## Host and translated guest baseline

Run from the repository root on the Linux ARM64 build host:

```sh
export NDK="$HOME/android-ndk-r29"
export NDK_HOST=linux-arm64
tools/prepare_dynarmic.sh
tools/extract_sysroot.sh
cmake -S . -B build/host -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
ninja -C build/host
tools/build_guest.sh
ctest --test-dir build/host --output-on-failure
tools/run_guest_tests.sh
```

`tools/run_guest_tests.sh` runs the checked-in guest suite. If the user has a
licensed Orange Roulette APK at `orange-roulette-1-0-0.apk`, it also runs the
optional `or_dlopen_dynamic` case; the APK is ignored and must not be committed.

Check committed generator outputs without rewriting them:

```sh
python3 tools/gen_jni.py --check
python3 tools/gen_egl.py --check
python3 tools/gen_gles.py --check
```

`tools/gen_syscalls.py` and `tools/gen_stubs.py` are write-only generators, not
check commands. Run them only with the pinned NDK and review their output diff;
different NDK headers can change generated syscall/stub files.

## Android runtime link and launcher APK

```sh
export NDK="$HOME/android-ndk-r29"
export NDK_HOST=linux-arm64
mkdir -p build/boost-headers
ln -sfn /usr/include/boost build/boost-headers/boost
cmake -S . -B build/android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=Release \
  -DZB_BUILD_TESTS=OFF \
  -DBoost_INCLUDE_DIR="$PWD/build/boost-headers"
ninja -C build/android-arm64 zbridge zbproxy zbjni_reflection_compile_test
python3 tools/check_zbridge_natives.py
python3 tools/check_zbproxy.py build/android-arm64/core/libzbproxy.so

tools/make_launcher_bundle.sh
cd android/launcher
./gradlew :app:assembleDebug
```

An Android link or APK build proves compilation only; device/runtime acceptance is
recorded separately in `plan.md`. AndroidIDE may inject
`com.itsaky.androidide.logwire.LogWireInitializer` into a debug manifest. Remove
that provider from AndroidIDE-built diagnostic APKs before installing them; do not
copy injected debug-manifest changes into product source.

## Retired root handoffs

There are no checked-in CI workflows or nested `AGENTS.md`/`CLAUDE.md` files. The
README links to this document and `plan.md`. Old mentions in archived plans and
reviews preserve historical context only; no build script, CI job, or current
developer workflow requires the deleted root handoffs.
