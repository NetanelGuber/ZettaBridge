# Build and developer workflow

`plan.md` is the canonical project handoff. This document contains repeatable
checkout, dependency, and baseline build commands. Historical implementation plans
under `docs/superpowers/` may mention the retired root `CLAUDE.md` or `AGENTS.md`;
those references are archival and are not build dependencies.

## Tested environment and prerequisites

The verified baseline is Ubuntu 24.04 x86-64 under WSL2, with an AArch64 host
cross-build run through QEMU user-mode emulation. The project host JNI thunk
assembly is AArch64-specific, so a native x86-64 host build is not supported. QEMU
host tests and translated guest tests are test evidence only; they are not native
device or Android runtime evidence. Keep the checkout in the Linux filesystem
(for example, `~/ZettaBridge`), not under `/mnt/c`.

Install the native and AArch64 cross-build tools:

```sh
sudo apt-get update
sudo apt-get install git clang ninja-build cmake openjdk-21-jdk python3 \
  libboost-dev curl unzip e2fsprogs gcc-aarch64-linux-gnu \
  g++-aarch64-linux-gnu qemu-user-static binfmt-support
```

The Android command-line tools used for the recorded baseline are the Linux
15859902 archive. Verify its published SHA-256 before unpacking:

```sh
export ANDROID_SDK_ROOT="$HOME/android-sdk"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
mkdir -p "$ANDROID_SDK_ROOT/cmdline-tools/latest" \
  "$HOME/.cache/android-cli-unpack"
curl --fail --location --retry 3 \
  --output "$HOME/.cache/commandlinetools-linux-15859902_latest.zip" \
  https://dl.google.com/android/repository/commandlinetools-linux-15859902_latest.zip
echo "4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583  $HOME/.cache/commandlinetools-linux-15859902_latest.zip" \
  | sha256sum --check
unzip -q -o "$HOME/.cache/commandlinetools-linux-15859902_latest.zip" \
  -d "$HOME/.cache/android-cli-unpack"
cp -a "$HOME/.cache/android-cli-unpack/cmdline-tools/." \
  "$ANDROID_SDK_ROOT/cmdline-tools/latest/"
```

Review and accept the Android SDK licenses, then install the pinned packages and
the launcher dependencies:

```sh
"$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager" \
  --sdk_root="$ANDROID_SDK_ROOT" --licenses
"$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager" \
  --sdk_root="$ANDROID_SDK_ROOT" \
  "cmake;3.22.1" "ndk;29.0.14206865" "platforms;android-35" \
  "build-tools;35.0.0" "build-tools;34.0.0" "platform-tools"
```

Review and accept the GSI terms on Google's official release page before running
the extractor. The extractor downloads the pinned direct URL and cannot display or
record page acceptance.

## Repository and toolchain

- Keep the working branch `main` tracking the personal fork's `origin/main`.
  `upstream` is the original project (`ZailoxTT/ZettaBridge`); never push there.
- The WSL2 NDK r29 package provides the `linux-x86_64` prebuilt toolchain. Set
  `NDK` and `NDK_HOST=linux-x86_64` explicitly for guest builds, generators, and
  Android builds.
- Build the Linux AArch64 host with `aarch64-linux-gnu-gcc` and
  `aarch64-linux-gnu-g++`. `qemu-user-static` and its registered `qemu-aarch64`
  binfmt handler execute those host test binaries on x86-64. Set
  `QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` so QEMU can find the cross libraries.
- The recorded baseline uses SDK CMake 3.22.1, Android NDK
  `29.0.14206865`, Android SDK platform 35, and build-tools 35.0.0. The launcher
  uses Java 21, Gradle wrapper 8.11.1, Android Gradle Plugin 8.7.3, and may also
  require build-tools 34.0.0 and platform-tools.
- The system Boost development package supplies host headers. Android CMake needs
  a directory containing only a `boost/` header link; create it under ignored
  `build/` as shown below.

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
export NDK="$ANDROID_SDK_ROOT/ndk/29.0.14206865"
export NDK_HOST=linux-x86_64
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

Run from the repository root in WSL2 Ubuntu 24.04. The current host runtime and
guest test suite require the extracted sysroot:

```sh
export NDK="$ANDROID_SDK_ROOT/ndk/29.0.14206865"
export NDK_HOST=linux-x86_64
tools/prepare_dynarmic.sh
tools/extract_sysroot.sh
tools/build_guest.sh
cmake -S . -B build/host-aarch64 -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DBoost_INCLUDE_DIR=/usr/include
ninja -C build/host-aarch64
NDK="$NDK" NDK_HOST="$NDK_HOST" \
  QEMU_LD_PREFIX=/usr/aarch64-linux-gnu \
  ctest --test-dir build/host-aarch64 --output-on-failure
NDK="$NDK" NDK_HOST="$NDK_HOST" \
  QEMU_LD_PREFIX=/usr/aarch64-linux-gnu \
  ZBRUN="$PWD/build/host-aarch64/cli/zbrun/zbrun" \
  ZBFIX="$PWD/build/host-aarch64/cli/zbfix/zbfix" \
  tools/run_guest_tests.sh
```

The guest suite skips `or_dlopen_dynamic` when the optional licensed
`orange-roulette-1-0-0.apk` is not present. The APK is ignored and must not be
committed. The host CTest invocation also runs the three generator consistency
checks using the selected NDK headers.

## Android runtime link and launcher APK

```sh
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
ANDROID_HOME="$ANDROID_SDK_ROOT" ./gradlew :app:assembleDebug --no-daemon
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
