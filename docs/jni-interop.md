# JNI and ART interoperability (Step 08)

## Claimed transitions

Converted, installed packages use their own ART `Context`, class loader and UID. A
bootstrap provider runs in each ordinary declared process before the source
providers. It activates the guest runtime before a source provider calls
`System.loadLibrary`. Providers have distinct Java classes and authorities so
ART initializes each process independently. A later dynamic native load uses
the already active runtime. The fixture covers the default process and a
`:worker` service process; each loads its ARM32 library through the generated
ARM64 proxy. The converter supports up to eight ordinary processes and rejects
an isolated or external service.

The native transition uses the Java calling thread as its carrier. Each
Java-to-guest call opens a host ART local frame and a guest handle frame. Nested
calls keep the outer frame, object returns survive `PopLocalFrame`, and guest
locals become invalid at return. Global and weak references use thread-safe
tables; weak references can clear after collection. Reused handles have a
generation, and slots retire before that generation can wrap. Invalid or
unmapped references fail explicitly rather than becoming host pointers.

The guest JNI table covers `JNIEnv` and `JavaVM` calls for objects, primitive
and object values, all primitive arrays, strings, monitors, exceptions,
references, `RegisterNatives`, `UnregisterNatives` and direct buffers. The
loader supports `JNI_OnLoad` and matching `Java_*` exports. Registration and
rebinding use typed thunks and parsed signatures, including mixed wide and
floating point arguments. `GetStringUTFRegion` copies encoded bytes without
writing a terminator. Array and string element or critical APIs use guest
copies and honor the release mode. Guest-created direct buffers must name a
mapped writable guest range. Java-created direct buffers use a bounded guest
mirror and flush at Java native-call return; a pending guest exception is
preserved across that flush. Guest-created threads may attach or detach through
`JavaVM`; an unsupported attach version returns `JNI_EVERSION` without attaching.

## Limits reported to callers

- A custom `Application` class initializer or `attachBaseContext` runs before
  providers. The preflight emits `early_application_load` for manual review;
  native loads there are outside this bootstrap contract.
- Isolated or external services cannot use the package-private guest runtime.
  Preflight reports `isolated_process`, and conversion rejects them.
- A Java-owned direct buffer is a copy: writes become visible at native return,
  not continuously. A later address request refreshes from Java and may replace
  guest writes. Mirroring is bounded; a buffer that cannot be mirrored returns
  null with a diagnostic.
- JNI critical regions are copies, not pinned ART memory. Code that depends on
  pinning or writing a Java-owned direct buffer asynchronously needs separate
  validation. Unsupported native APIs and guest libraries remain subject to
  the loader and preflight reports; this step makes no claim for arbitrary APKs.

## Step 08 evidence (2026-09-25)

- WSL2 AArch64 host build: `jni_bridge_test`, `jni_loader_test` and
  `guest_jni_engine_test` built. Under `qemu-aarch64-static`, all three passed,
  including both engine load and expected preload-failure modes. Focused bridge
  probes covered every primitive return and field type, varargs and `jvalue`
  calls, nested Java callbacks, local/global/weak references and GC, strings,
  arrays, monitors, exceptions, registration and rebinding, two guest threads,
  attach/detach, and direct-buffer validation and mirror flush. Invalid handles
  and unmapped pointers produced the expected controlled fatal result.
- `jni_handles_test` passed under QEMU with 5,000 reuse cycles on a local and
  global slot and a one-million-reference LIFO case. The AArch64 `jni_abi_test`,
  `jni_descriptor_test`, `jni_mangle_test`, `jni_shorty_test` and
  `proxy_runtime_test` passed under QEMU. `tools/gen_jni.py --check` passed
  against NDK 29 with `NDK_HOST=linux-x86_64`.
- Python converter tests: 5 passed; preflight tests: 8 passed. The Android ARM64
  `zbridge` and `zbproxy` builds passed, as did the proxy export/dependency and
  native registration structure checks. The Step 08 fixture and bootstrap APK
  Gradle builds passed.
- Pixel 11 Pro XL, Android API 37, serial `67161FDDV0011Q`: converted fixture
  APK SHA-256 `AA39FE560BAD9AC4AD630F1016564CB9F9F5AFDC061117FAEC17AE973CA084B5`,
  signer SHA-256 `d3fe5a914ad2f4139c645ae3a09ba845d484b2f946a50826326beb41d5575c00`.
  `adb install -r` succeeded. After force-stop and deleting both prior marker
  files, the activity started and newly written private marker files reported
  `step08-provider.txt: PASS` and `step08-worker.txt: PASS uid=10390 pid=14731`;
  the main process PID was 14691. This is synthetic device evidence for provider
  ordering and a distinct secondary process. The extensive JNI type and stress
  evidence above is QEMU/mock ART evidence, not a real app compatibility claim.
