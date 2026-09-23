# ZettaBridge Personal Fork: Completion Plan

**Plan version:** 1  
**Repository baseline reviewed:** main, 4acdf51c11118b1d9d04c2c117feab249ea51072 (v0.1.0)  
**Purpose:** Session-by-session implementation roadmap for a personal, root-managed fork.  
**Fork relationship:** This independent fork is maintained by a different person from the original project owner. It has no affiliation with, endorsement from, or operational relationship to the original project or its owner.

**Style:** English and ASCII in repository files.

## How to use this plan

Start a new session with a bounded request such as:

> Implement Step 04 from plan.md. Read plan.md and the current source relevant to it. Work only on Step 04, satisfy its acceptance criteria, record evidence and update its status in plan.md. Do not start later steps.

For each step:

1. Read this plan and current source files relevant to the step. This plan is the durable user-specific handoff for the fork.
2. Until Step 01's retirement gate is complete, CLAUDE.md and AGENTS.md may be consulted as legacy inputs only. Do not follow their superseded product scope or assume their dated status is current.
3. Check git status, branch/commit, build environment, and existing tests before editing.
4. Reconcile stale plan statements with current source; keep the user goal and step scope fixed unless a real blocker requires an ADR.
5. Run only the verification appropriate to the step; distinguish host, guest, build, rooted-device, and user-app evidence.
6. Update this plan's status/evidence, commit locally with an imperative English subject, and do not push upstream without explicit user authorization.

Statuses: NOT STARTED, IN PROGRESS, BLOCKED, DONE, DEFERRED. Compile success alone does not complete a step.

## 1. Goal and precise meaning of "64-bit APK"

Accept an ARM32-only Android APK, inspect it, produce a new APK Android can install on an ARM64-only phone, and install it through the root-enabled manager. The installed app is registered by Android PackageManager as its own package, process, permission principal, and UID.

The output is an **ARM64-installable APK**, not an offline rewrite of every ARM32 instruction into AArch64. The Java/Kotlin DEX, resources, and manifest run on the device's real 64-bit ART. The ARM32 native libraries remain guest code and execute through ZettaBridge's A32/T32 dynamic translator inside that installed app process. ARM64 proxy libraries connect normal Android library-loading entry points to the guest runtime.

### Root and app identity contract

- Root is limited to management actions: validate/stage selected inputs, invoke PackageManager install/remove when the user explicitly chooses, and optionally collect authorized backups/diagnostics. KernelSU is the initial root provider; keep an interface seam for other su providers.
- Guest Java, native libraries, and scripts always run as the normal UID assigned to the converted package by Android. Never run imported app code as root. The root component must not become a general-purpose app runtime.
- The installed package has a normal Android package registration and UID sandbox. It does not retain the source APK's signing identity.
- Repacking changes the signature. Sign with one stable personal key. The source signing certificate/private key cannot be preserved or reconstructed from the APK. Apps that check their certificate, use signature permissions/shared UID, rely on certificate-registered APIs, enforce integrity/anti-tamper, or require the original signer for updates may fail. Root does not override signature checks.
- Keep the source package name by default. If an original-signed copy conflicts, stop and explain the signing mismatch. Never silently uninstall it, overwrite its data, or claim that changing the package suffix preserves identity. A renamed copy can be a later explicit option with provider/authority/deep-link/API-registration/data consequences explained.
- Root is part of this personal product's privileged installation/management workflow; it is not technically necessary for executing a correctly signed ARM64 APK and is not a compatibility bypass.

### Compatibility target and hard limits

Initial target: ARM64 Android devices with 64-bit-only userspace/kernel configurations; ARM32 guest ABIs armeabi and armeabi-v7a. Grow support by instruction, linker, syscall, JNI, Android API, and graphics/media feature, not by APK-name special cases.

"Almost all" is a goal, never a proof claim. Some apps cannot be made equivalent without their original key, source, server support, vendor library, hardware feature, or a compatible 32-bit platform service. DRM, Play Integrity, deliberate anti-emulation, undocumented syscalls, unsupported ISA/device features, missing vendor services, and code relying on signing identity remain hard boundaries. Do not bypass security controls to claim compatibility.

The minimum broad target is routine Java/ART apps with ARM32 JNI, common native APIs, resources/assets, standard Android components, GLES/EGL, audio, and common input/event paths. NativeActivity/pure-NDK apps and Vulkan matter to broad coverage and are separate, substantial steps. Unsupported APIs must be reported rather than silently returning success.

## 2. Architecture contract

Primary design: **per-app APK conversion with an embedded runtime**.

1. Analyze the source APK or complete split set without executing it.
2. Preserve DEX, resources, package metadata, and app behavior where possible.
3. Move ARM32 ELF libraries into a guest-only package area so the ARM64 system linker never loads them as host code.
4. Add ARM64 ZettaBridge runtime/support libraries and ARM64 proxies for expected guest library names.
5. At runtime, a proxy initializes the guest process, loads ARM32 bionic/sysroot and guest ELF libraries, translates A32/T32, and routes JNI/platform calls through the bridge.
6. Sign and verify the finished package with the user's personal key.
7. Use root only for an explicitly selected PackageManager operation; Android runs the app under its regular per-package UID.

The existing launcher/plugin runtime is valuable implementation to adapt, but a plugin/shortcut does not meet installed identity requirements.

An early architecture spike must validate proxy loading for Java System.loadLibrary and NativeActivity in an ordinary installed package. If that model proves structurally impossible on the target Android version, record an ADR before changing direction. AOSP NativeBridgeCallbacks is a comparison/fallback, not the default deliverable: it is system integration using ART callbacks and a separate guest linker, rather than a generated per-app APK. Do not start boot/system/ART modification without an ADR.

## 3. Upstream project guidance retained; its old product scope replaced

### Keep these engineering constraints from CLAUDE.md and AGENTS.md

- Guest Java/Kotlin runs on native 64-bit ART; translate guest native code only. Do not add 32-bit ART, custom ROM, or restored 32-bit userspace requirements.
- Use real ARM32 Android bionic/linker and translate the ARM32 Linux syscall ABI. Host 64-bit libc is not a drop-in replacement.
- Guest pointers are 32-bit offsets inside the reserved 4 GiB guest memory mapping. Translate/copy at boundaries or use checked 32-bit handles; do not expose arbitrary host pointers. Check lengths, overflow, and guest memory ranges.
- Preserve Dynarmic callback discipline: record a stop and halt in the callback; do host/platform work after translated execution returns to the dispatcher.
- Preserve syscall, signal, TLS, guest thread, borrower/carrier, JNI, and AAPCS32 behavior. JNI thunks must respect guest ABI (including softfp argument placement and aligned 64-bit values); ART signal handling must remain correct.
- Keep generated GLES/EGL/JNI/syscall/stub files generated from declared sources. Edit generators/source data, not generated output. Host-call IDs are append-only; inspect current allocation tables before adding entries.
- Keep loaders, API surfaces and compatibility fixes generic. Orange Roulette, Flappy Bird and Flutter are evidence/bring-up cases, not product definitions. Never hard-code app names, package IDs, engines or one APK's import list.
- The local Dynarmic patches are a required baseline until deliberately reviewed: third_party/patches/dynarmic-0001-thumb32-armv8.patch and dynarmic-0002-asimd-narrowing.patch. Do not accidentally stage a Dynarmic submodule pointer change or lose the documented patch workflow.
- QEMU can be a developer reference, not a linked/shipped dependency. Check dependency licenses and keep notices.
- Provide actionable device reports because OxygenOS may omit third-party logs from logcat. Record build hashes and device/build conditions for actual device evidence.
- Preserve the upstream project's AArch64 Linux + clang/CMake/Ninja + Boost headers + Android NDK r29 build path. A clean setup initializes submodules, obtains/extracts the Android 17 arm64 GSI guest sysroot, applies the local Dynarmic patches, then builds host/guest/Android targets. Harden scripts for idempotence, integrity, storage use and downloads.
- Established checks include host CMake/Ninja, CTest, tools/build_guest.sh, tools/run_guest_tests.sh, generated-file checks, Android zbridge/zbproxy links, runtime bundle validation, and Gradle APK assembly. Check current target names and counts; historical counts are not promises.
- AndroidIDE may inject LogWireInitializer into debug manifests; use the inherited manifest cleanup guidance if that build path is still used. Never overwrite Gradle wrapper files.
- Commit bounded tasks locally. Push only with explicit user authorization.

### Product decisions for this fork that supersede upstream guidance

The original project intentionally chose a non-root plugin launcher: no APK repackaging, no installed guest package identity, and no system-wide bridge. This independent fork has a separate owner and product direction; the fork owner chose the per-app conversion goal recorded here. This plan authorizes work in this checkout only, not edits to or pushes to the original project. Retain compatible translator architecture and required notices.

AGENTS.md and CLAUDE.md are inherited upstream handoffs, not current affiliation, ownership, or fork-status statements. Reconcile their dated details with this checkout before implementation. The fork began from original upstream main at 4acdf51; create a personal codex/* branch before product edits and keep the original remote untouched absent explicit push authorization.

### License and source hygiene

The inherited repository states cumulative PolyForm Noncommercial and PolyForm Perimeter terms and is source-available, not unrestricted open source. This independent fork retains upstream notices but is not affiliated with or endorsed by the original project owner. Check all added and bundled dependency licenses; do not turn it into a service or redistribute builds without separately reviewing permissions. Do not put APKs the user may not redistribute in Git/release artifacts. Do not submit this fork upstream without addressing the repository's contributor copyright-assignment terms.

## 4. Reviewed starting point and known gaps

- The baseline already has a substantial A32/T32 translator, ARM32 sysroot/linker path, syscall layer, library-mode runtime, JNI bridge, launcher/plugin import path, generated GLES/EGL APIs, and Android build packaging.
- Inherited upstream README and acceptance docs report Orange Roulette and Flappy Bird playable on the original project owner's OnePlus 13. These are upstream owner's historical results, not device validation by this fork. The records also report a Flutter guest launching, running Dart, presenting frames and accepting touch, with incomplete rendering.
- The newest AGENTS.md handoff says Flutter text and some raster images are missing. Prior upload/UBO and legacy-alpha hypotheses were ruled out; a GL_R8/GL_RED A/B was reverted because text did not improve. Its next lead was instrumenting an actual onscreen text draw. Verify current source before acting; do not mistake this one guest for broad compatibility proof.
- Inherited upstream records say NativeActivity is unimplemented and list NativeActivity/Unity/pure-NDK, Vulkan, and apps requiring a real package installation as unsupported in the original product. Recheck these claims in this fork before using them as current status.
- The fork started from upstream main at `4acdf51`; old branch names such as `codex/phase4d-launcher` are upstream history, not fork branches or affiliation.
- No code, build, tests, conversion, or rooted-device run was performed in this planning task. This document is a roadmap, not an implementation validation report.

## 5. Numbered implementation steps

### Step 00 - Freeze scope and architecture decision

**Status:** DONE

**Goal:** Record a short ADR before product implementation.

**Tasks**
- Confirm the embedded per-app runtime/proxy APK model and why plugin-only execution fails the goal.
- Record identity/signing constraints, initial input formats, root boundary, device/API/ABI/root-provider baseline, SELinux state, graphics/API capabilities, and storage constraints from the actual phone.
- Inspect current proxy, runtime bundle and launcher assumptions; identify what can be reused in an installed package.
- Design the smallest synthetic APK plus ARM32 library proof; no named commercial app is required.
- Define the go/no-go check for proxy loading, JNI_OnLoad, guest sysroot access, PackageManager install and NativeActivity bootstrap.
- Compare NativeBridgeCallbacks in the ADR only. Do not start system modification without a new decision.

**Done when:** The ADR gives the data flow, trust boundary, signing/identity behavior, supported input, device target, explicit exclusions, biggest risk and smallest resolving experiment. This step edits documentation only.

**Depends on:** none.

**Evidence (2026-09-23):** The architecture decision, source audit, smallest synthetic
proof, and go/no-go checks are in [ADR 0001](docs/adr/0001-per-app-runtime.md).
Source review used baseline `4acdf51c11118b1d9d04c2c117feab249ea51072`. Live ADB
inventory was captured from Pixel 11 Pro XL serial `67161FDDV0011Q`: Android 17/API
37, ARM64-only advertised ABI, SELinux enforcing, KernelSU 3.3.0 installed, GLES
3.2 and Vulkan 1.4 advertised, and 414 GB available on the data filesystem. The ADR
distinguishes these capability readings from unrun app-context proxy/JNI/sysroot/JIT
and NativeActivity acceptance tests. This was documentation-only; no product source,
build, or tests were changed/run. Step 01 remains NOT STARTED.

### Step 01 - Personal-fork handoff and reproducible baseline

**Status:** NOT STARTED  
**Tasks**
- Create a personal codex/* branch; confirm `origin` still points to the original project and do not push to it without explicit authorization.
- Record commit, submodule SHA/patch state and compiler/SDK/NDK/CMake/Java versions.
- Make plan.md the canonical handoff. The durable architecture, ABI, security, licensing, build, diagnostic and workflow requirements from the inherited upstream files have been summarized above; check both files once more for any still-actionable requirement not represented here.
- Search build scripts, CI, nested instructions and docs for dependencies on root CLAUDE.md or AGENTS.md. Move any essential live build commands or workflow detail into this plan (or a focused non-agent-guide developer document), and remove obsolete/conflicting references.
- After the migration checks pass, delete CLAUDE.md and AGENTS.md from this personal fork. Do not alter or push those removals to the original upstream repository.
- Make submodule initialization and local patch application repeatable and detect already-applied patches/wrong revisions.
- Make GSI/sysroot extraction resumable, integrity-checked, size-aware, and clear about source/license. Keep generated sysroot out of Git unless required.
- Reproduce existing host/guest/Android checks before architectural changes. Review ignore rules and generated/bundle checks.

**Done when:** A clean checkout can follow documented steps to build host, guest tests and Android runtime targets without changing the Dynarmic pointer; baseline commands/results are recorded on a personal branch; no build/CI/tooling process requires CLAUDE.md or AGENTS.md; the fork copies of both files are removed after their remaining actionable content is migrated.

**Depends on:** Step 00.

### Step 02 - APK/split analyzer and preflight report

**Status:** NOT STARTED  
**Tasks**
- Read-only inspection of package/version, min/target SDK, manifest/components/permissions, signer digest, ABIs, ELF class/machine/attributes, DT_NEEDED, symbol imports/exports, JNI hints, native entry points, compression/alignment and split relationships.
- Support one APK and a validated complete split set (including common .apks containers if feasible). Reject incomplete/mixed-version/mixed-signer sets. AAB input is outside the first release unless explicitly added.
- Defend against ZIP bombs, path traversal, duplicate dangerous entries, symlinks, malformed ELF/manifest, excessive file count and oversized decompressed content.
- Detect ARM32/mixed/Java-only/x86 packages, NativeActivity, shared UID, signature permissions, dynamic feature splits, embedded APKs and relevant graphics declarations.
- Report each gap as convertible, warning, or unsupported with a reason. Never execute input during analysis.

**Done when:** Synthetic valid, malformed, ambiguous, oversized, split and multi-ABI fixtures classify safely; report separates install/signing blockers from runtime gaps; source is unchanged.

**Depends on:** Step 01.

### Step 03 - Narrow root manager

**Status:** NOT STARTED  
**Tasks**
- Implement a KernelSU/su backend with explicit grant state, cancellation, timeout, fixed operations and useful failures; keep a seam for later providers.
- Limit root to verifying provider, safely staging a user-selected input, invoking explicitly authorized PackageManager actions, and collecting explicitly requested backups/diagnostics.
- Use file descriptors, validated fixed arguments, canonical paths, symlink-resistant staging and hashes. Never interpolate user-controlled paths/text into shell commands.
- Run analysis/translation as an unprivileged manager operation and guest execution only in the output app's Android-assigned UID.
- Do not modify ro.dalvik.vm.*, ART/system image, SELinux policy, system partitions, or run imported code as root.
- Confirm install/replace/uninstall/data operations separately and show consequences.

**Done when:** Root denial/cancel/timeout/path replacement/command failure all fail closed; the backend cannot launch guest code as UID 0; installed apps retain normal UID sandbox.

**Depends on:** Steps 00-01.

### Step 04 - Runtime bootstrap for ordinary installed apps

**Status:** NOT STARTED  
**Tasks**
- Audit libzbridge.so, libzbproxy.so, LibraryRuntime, JNI and runtime bundle for assumptions about launcher process, :guest, DexClassLoader, shortcut context, plugin paths and reports.
- Define stable per-app bootstrap config, guest library/assets path, private cache, backend choice and startup diagnostics.
- Generate/package ARM64 proxy libs named for expected ARM32 libraries; handle one-time bridge startup, JNI_OnLoad, Java_* exports, RegisterNatives, guest dlopen/dlsym and process lifetime.
- Cover recursive/multiple library loads, multiple guest processes, reentry, failure and orderly teardown/reporting.
- Keep sysroot read-only; put JIT cache and extracted data in app-private storage. Prove app-UID access without root-only runtime paths.

**Done when:** Minimal installed ARM64 test package loads an injected proxy, calls a synthetic ARM32 library, and safely calls back to ART without the ZettaBridge launcher, shared UID, clone sandbox or guest root.

**Depends on:** Steps 00-01.

### Step 05 - APK transformation, signing and verification

**Status:** NOT STARTED  
**Tasks**
- Build a transactional pipeline: private staging -> analyze -> transformation manifest -> new output -> structural verification -> sign -> signature verification -> atomic publish. Never edit source input.
- Preserve DEX/resources/manifest/package name where possible; record every manifest/resource change.
- Move ARM32 ELFs into guest-only assets; include ARM64 runtime/support libraries and ARM64 proxies. Android must not attempt to load guest ELF as native.
- Handle splits, compression/alignment, duplicate names, library mapping and app-specific guest data.
- Inject bootstrap config/proxies for System.loadLibrary and the entry points implemented in later steps; avoid textual binary-manifest editing.
- Create/import one stable personal signing key securely; never log/export key material. Sign only after edits, then verify. Record signer fingerprint and input/output hashes.
- Bound and clean unsigned intermediates and failure state.

**Done when:** Synthetic output passes APK/signature checks and PackageManager accepts it as ARM64; proxy libs are ARM64, guest libs are not host-loadable, failures preserve original input and installed apps.

**Depends on:** Steps 02, 04.

### Step 06 - Safe PackageManager install, update, remove and recovery

**Status:** NOT STARTED  
**Tasks**
- Use Android PackageManager/PackageInstaller semantics, root only for authorized invocation/result. Never forge package-manager state or hand-copy files into /data/app.
- Verify output package name, signer, version, ABI, process and Android-assigned UID.
- Detect original-signer conflict before any removal. Offer cancel/keep original or an explicit, safely implemented renamed copy. Never remove/replace original or erase data by default.
- Back up conversion metadata, hashes, key fingerprint and prior version; document what data backup can/cannot restore, especially encrypted or certificate-bound state.
- Define rollback and behavior across users/work profiles. Restrict initial release scope honestly if only one Android user is supported.

**Done when:** Synthetic app appears in system app lists, launches normally, has its own PackageManager UID and permission prompts; same-personal-key update works; conflict and failure preserve original/data; guest never runs root.

**Depends on:** Steps 03, 05.

### Step 07 - ELF loader, libraries and guest sysroot

**Status:** NOT STARTED  
**Tasks**
- Harden ARM32 ELF parsing, relocations, symbol/version lookup, constructors/destructors, TLS, GNU hash, unwind/EXIDX, RELRO, weak symbols and binding modes.
- Define guest library search order across APK guest assets, app-private extracted files, Android 17 guest sysroot and explicitly supported platform/vendor libraries. Never search host ABI paths.
- Generalize legacy absolute DT_NEEDED and DT_TEXTREL fixups; constrain and audit writable-code behavior.
- Implement required dlopen/dlsym/dladdr/dlerror flags, recursive dependency/load/unload behavior.
- Expand API-level library compatibility; Android 17 sysroot is a baseline, not proof of old/vendor app coverage.
- Map unresolved libraries/symbols into static preflight output.

**Done when:** Isolated guest fixtures cover claimed loader behaviors and failures; reports distinguish missing lib/symbol, ABI mismatch, relocation error and execution fault; loader cannot escape guest namespace.

**Depends on:** Steps 04-05.

### Step 08 - JNI and ART interoperability

**Status:** NOT STARTED  
**Tasks**
- Audit JNIEnv/JavaVM, references/handles, JNI_OnLoad, RegisterNatives, Java_* exports, method rebinding, exceptions, monitors, strings/arrays, direct buffers, critical regions, attach/detach, class loaders and multiple Java threads.
- Bootstrap against the installed package's ART class loader/context; handle Application and ContentProvider startup ordering, secondary/isolated processes and dynamic native-load paths.
- Preserve JNI transition references/local frames correctly; keep GC, carrier-thread, ABI and signal invariants.
- Expand mock JVM and translated ARM32 JNI probes for all implemented types, nested calls, multithread registration, exceptions and races.
- Report unsupported patterns precisely.

**Done when:** Generated JNI checks and host/guest probes pass across claimed transitions; no wrong ABI args, stale handles, unsafe pointer exposure or ART reference misuse in focused stress checks.

**Depends on:** Steps 04, 07.

### Step 09 - Android component and lifecycle integration

**Status:** NOT STARTED  
**Tasks**
- Preserve/test activities, aliases, services, receivers, providers, task affinity/launch modes, orientation, themes, metadata, permissions, intent filters, process names and configuration changes.
- Make bootstrap ready before ContentProvider/Application/native library startup.
- Resolve package/resource/content URIs as the installed package; do not spoof other-package queries or permissions.
- Cover secondary processes, process death/restart, force-stop, task recents, deep links, notifications, alarms, file providers and component visibility.
- Preflight/report manifest features the converter cannot preserve.

**Done when:** Synthetic fixtures establish correct system-managed components, lifecycle, internal intents and process separation; no home-grown PackageManager emulation is used.

**Depends on:** Steps 05-08.

### Step 10 - NativeActivity and pure-NDK entry points

**Status:** NOT STARTED  
**Tasks**
- Add an ARM64 NativeActivity bootstrap library that Android loads and that invokes the ARM32 ANativeActivity_onCreate through the bridge.
- Marshal ANativeActivity callbacks, JNI, ANativeWindow, input queues/events, saved state, assets, configuration, intents, destroy and lifecycle with checked handles/lifetimes.
- Preserve callback thread/looper semantics; no fake success for callback or poll APIs.
- Support android.app.lib_name discovery and manifest setup; detect missing/incompatible entry point.
- Probe ordering, pause/resume, surface recreation, rotation, input, process death and reentrancy.

**Done when:** A synthetic NativeActivity guest reaches onCreate, gets real lifecycle/window/input callbacks, renders using supported graphics, and shuts down cleanly; every advertised callback works or fails explicitly.

**Depends on:** Steps 04, 06-08, 11-12.

### Step 11 - Common Android NDK platform APIs

**Status:** NOT STARTED  
**Tasks**
- Inventory current generated stubs and imports; classify each as implemented, explicit failure, unsupported or pending; preserve append-only host-call indices.
- Implement/test AAsset*, ANativeWindow, ALooper*, AInputQueue*, configuration, bitmap/graphics APIs and common callbacks.
- Verify current status of APIs mentioned in old Flutter handoffs: real looper callbacks, AndroidBitmap_*, EGLImage and platform surface calls.
- Add sensor, vibration, clipboard, storage/URI and permission bridges as needed without granting beyond Android policy.
- Define handle type, ownership, reference counts, errors and thread semantics. Generate mechanical surfaces from headers/specs, hand-audit pointer/lifetime cases.

**Done when:** Every supported API has contract coverage for lengths, pointers, invalid handles, ownership/threading/errors. Used-but-unsupported calls appear in reports; host pointers are never exposed.

**Depends on:** Steps 04, 08, 10.

### Step 12 - EGL/GLES correctness and renderer gaps

**Status:** NOT STARTED  
**Tasks**
- Audit generated GLES/EGL entry points, pointer/length marshaling, extension formats, error behavior, context/surface/share-group/object lifetime and thread affinity.
- Diagnose the inherited upstream Flutter onscreen text/raster problem from actual onscreen draws. The upstream handoff ruled out several UBO/upload/alpha theories and reverted the GL_R8/GL_RED experiment; do not repeat without new evidence or use offscreen warmups as proof.
- Audit mapped buffers, client arrays, compressed textures, BGRA/extensions, FBO/readback, shader logs, GLES3 calls, sync and multisampling.
- Expose only host-supported features or correct emulation; queue errors with guest semantics. Preserve generator checks and add overflow/pointer-boundary probes.
- Keep GL work on the host thread with current EGL context and report mismatches/rejections.

**Done when:** Host tests cover claimed GLES/EGL calls/errors/marshaling/lifetimes; rooted-device synthetic 2D/3D probes present, accept input, recreate surfaces and swap repeatedly. Known onscreen text/raster issue has evidence-based resolution. One guest is not the definition of general coverage.

**Depends on:** Steps 04, 08, 10-11.

### Step 13 - Vulkan backend

**Status:** NOT STARTED  
**Tasks**
- Prove a narrow guest Vulkan path: loader, instance/device, surface/swapchain, command buffer and frame presentation.
- Design ABI wrappers/handle tables for dispatchable and non-dispatchable handles; marshal nested structs, unions, arrays, pNext chains, callbacks, allocator hooks and opaque values.
- Use the ARM64 device Vulkan loader/driver, never an APK's ARM32 driver.
- Cover Android surface, queue sync, memory mapping, fences/semaphores, errors/device loss, API versions and extensions.
- Generate mechanical wrappers from registry where safe; hand-audit pointer graphs/callback lifetimes. Publish a capability/preflight matrix.

**Done when:** Synthetic ABI/sync tests and rooted-device probe present/recreate a Vulkan surface via real ARM64 driver; unavailable extensions fail accurately.

**Depends on:** Steps 04, 08, 10-11.

### Step 14 - Audio, media, camera and common device features

**Status:** NOT STARTED  
**Tasks**
- Inventory OpenSL ES, AAudio, AudioTrack/JNI, MediaCodec, image decode, camera, sensors, vibration, input and display APIs.
- Route audio through host-supported APIs with format/rate/channel conversion, latency control, attached callback threads and lifecycle-safe release.
- Bridge codec/surface buffers, timestamps, sync, format changes, permissions and errors.
- Use Android permission/capability APIs for camera/sensors; preserve user prompts/revocation.
- Support touch, keyboard, game controllers, orientation, lifecycle, display and NDK event layouts. Report absent host services/features.

**Done when:** Each claimed service has synthetic handle/buffer/callback/permission/error checks; denial/missing hardware never grants guest root and resources release on pause/death/recreation.

**Depends on:** Steps 08, 10-12; Vulkan surface path also depends on Step 13.

### Step 15 - Broaden ARM32 ISA, syscalls and historical Android behavior

**Status:** NOT STARTED  
**Tasks**
- Audit ARMv5/6/7-A A32/T32, VFP/NEON/ASIMD, softfp/hardfp, atomics/barriers and self-modifying code. Use exact disassembly and assembler probes for undefined instructions.
- Add minimal isolated guest regression before translator changes; keep Dynarmic patch set reviewable and license-compatible.
- Expand ARM EABI syscalls with correct 32-bit layouts: time/stat variants, ioctl, signals, futex, mmap, prctl, filesystem, sockets and overflow/bad-memory behavior. Never pass differing ARM32 structs directly to kernel.
- Improve guest proc/dev/auxv/CPU features, linker config, API-level libraries and property behavior without exposing sensitive host state.
- Revisit currently refused fork/vfork/execve/ptrace individually. Do not execute extracted code from app-writable paths or weaken SELinux to claim compatibility.
- Keep precise guest faults as a diagnostic/per-app option due measured performance cost; validate actual PC/registers.
- Track PI futex/carrier, vendor system libraries, custom kernels and unavailable services.

**Done when:** Focused host/guest regressions cover each ABI/syscall addition and invalid boundaries; precise mode identifies faulting PC/registers; unsupported operations fail with stable diagnostics without corrupting the host.

**Depends on:** Steps 07-08, 11.

### Step 16 - Performance, resource bounds and stability

**Status:** NOT STARTED  
**Tasks**
- Measure startup, JIT, integer/floating-point, memory copies, JNI, syscalls, graphics/media boundaries, memory, cache and APK size on target.
- Optimize measured hot paths without violating ordering, callbacks, errors, threading or context rules. Batch only with correctness evidence.
- Bound guest memory, threads/JITs, caches, reports, extracted assets, temporary storage and output size; implement cleanup/cancellation/low-storage behavior.
- Handle interrupted conversion/install, thermal/battery pressure, crash reporting and process restarts. Keep failures isolated from manager/other apps.
- Keep telemetry local and privacy-preserving; never upload APKs, keys, app data or reports.

**Done when:** Before/after measurements justify changes; correctness probes do not regress; resource exhaustion/interruption is diagnosable and recoverable.

**Depends on:** Steps 05-15 as applicable.

### Step 17 - Generic compatibility evidence and reports

**Status:** NOT STARTED  
**Tasks**
- Maintain a machine-readable matrix for ISA/ABI, syscalls, loader, JNI, components, NDK, GLES/EGL, Vulkan, audio/media and identity/signing constraints.
- Prefer synthetic probes, host mocks and generated import/API coverage over a named-APK catalogue.
- Emit static preflight and bounded runtime report: missing symbols, calls/syscalls, bridge rejections, guest exit/fault, graphics/media errors and installed package/UID/signing result.
- Label evidence: static analysis, host test, translated guest probe, Android build/link, rooted-device synthetic probe, optional user-selected APK smoke.
- Keep user-owned/commercial APK testing manual and opt-in. No finite APK suite proves almost-universal compatibility; do not hard-code chosen test apps.
- Do not require APK uploads or app data for bug reports.

**Done when:** Each failure maps to a feature and guest location where feasible; reports distinguish observed from unverified; user can test chosen APKs without making them product special cases.

**Depends on:** Steps 02-16, incrementally.

### Step 18 - Personal release and finish criteria

**Status:** NOT STARTED  
**Tasks**
- Produce manager/root component and converted APK from clean documented setup. Record version, source/output hashes, key fingerprint, device/API, root provider and known limits.
- Document secure key creation/backup/restore and loss implications; never embed private key in APK/repository.
- Document install/update/remove, original signer conflict, backup/data-loss limits, root/UID behavior, unsupported features and recovery to source app.
- Review archive parsing, root IPC, temp files, signer flow, exported components and untrusted APK boundaries. Preserve licenses/notices and symbols needed for diagnosis.
- Store artifacts/conversion metadata locally; do not publish or push without authorization.

**Finish criteria**
- Clean documented build produces the manager and embedded runtime.
- Rooted ARM64-only device can analyze, convert, verify, install, launch, update and remove a supported APK/split set as a normal package with its own Android UID.
- Java runs on native ART; ARM32 native code executes through translation; unsupported requirements produce useful preflight/runtime diagnostics.
- Generic probes pass for advertised features. User's few chosen APKs may be manual integration checks but are not a claim of universal coverage.
- Root is confined to management; installed apps run in the Android sandbox.
- Signing constraints, hardware/API gaps, Vulkan status, performance and unsupported behavior are honest.
- Describe the result as broad ARM32 compatibility on a stated device/configuration, never as compatibility with all 32-bit APKs.

Vulkan and specific hardware APIs may be unsupported in an early personal release, but then the result is not near-universal for apps requiring them.

**Depends on:** all required steps.

## 6. Cross-step completion checklist

Before marking any step DONE:

- [ ] Acceptance criteria were checked against current source and toolchain.
- [ ] No test/build/device result is claimed unless actually obtained; host tests do not prove device behavior.
- [ ] Root, UID, package signing and untrusted input boundaries were addressed where relevant.
- [ ] Generated files, local Dynarmic patches and append-only host-call allocations remain consistent.
- [ ] Failure paths are actionable and preserve source APK and installed data.
- [ ] This plan's status/evidence/next step are updated and implementation is committed locally.
- [ ] No upstream push or third-party redistribution occurred without authorization.

## 7. Primary references

Re-check current platform behavior while implementing:

- [AOSP Native Bridge overview](https://android.googlesource.com/platform/art/%2B/9792f354eae585aef426e05b474683ceb50f3384/libnativebridge/README.md): host-ISA managed process with a separate linker for translated foreign-ISA libraries.
- [AOSP Native Bridge callback interface](https://android.googlesource.com/platform/art/%2B/cea2f596f9/libnativebridge/include/nativebridge/native_bridge.h): versioned callback surface.
- [Android app signing](https://developer.android.com/studio/publish/app-signing): update signing-key continuity and package update constraints.
- [AOSP application sandbox](https://source.android.com/docs/security/app-sandbox): unique app UID and kernel-level app isolation.
- [AOSP security implementation guidance](https://source.android.com/docs/security/overview/implement): keep privileged/root services narrow and isolated.
