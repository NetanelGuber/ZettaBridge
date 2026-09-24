# Guest ELF and linker contract (Step 07)

ZettaBridge's small host ELF loader maps the ARM32 service executable and its ARM32
interpreter. It checks the ELF class, machine, headers, segment ranges, alignment,
entry and readable program headers before execution. It leaves holes unmapped and
combines permissions for pages shared by segments. It does not relocate app libraries.
The ARM32 Android 17 bionic linker performs dependency loading, relocations,
symbol and version lookup, constructors and destructors, TLS, GNU hash, RELRO,
unwind/EXIDX, and guest `dlopen`/`dlsym`/`dladdr`/`dlerror` behavior.

## Search and execution boundary

The runtime sets `LD_LIBRARY_PATH` to the bundled ARM32 support-library directory,
then the converted app's private ARM32 library directory. Source APK libraries
must be extracted there; compressed APK members are not linker paths. Present
ARM32 sysroot directories follow in this order: `system/lib`, `system_ext/lib`,
`product/lib`, `vendor/lib`, `odm/lib`.
Guest `/system`, `/system_ext`, `/product`, `/vendor`, `/odm`, `/apex`, and
`/linkerconfig` paths map to that sysroot. The runtime APEX bionic path maps to
the extracted `/system/lib` or `/system/bin` bootstrap copy. Vendor and platform
libraries are available only when explicitly present in the guest sysroot. There
is no host ABI library fallback. Missing font data (`.ttf`, `.ttc`, `.otf`, and
`/system/etc/fonts.xml`) may use the device's architecture independent files.

System-path traversal and escaping symlinks are denied. File-backed executable
guest mappings require a little-endian ARM ELF32 file under the sysroot or one of
the declared ARM32 `LD_LIBRARY_PATH` roots. Later `mprotect(PROT_EXEC)` is subject
to the same page policy. Anonymous executable mappings remain available for guest
JITs. This bounds the built-in guest linker; it is not a sandbox against arbitrary
guest code copying bytes into its own anonymous executable mapping.

## Legacy libraries and diagnostics

The import fixer basenames legacy absolute `DT_NEEDED` strings in place and rejects
an empty basename. It changes `DT_TEXTREL`/`DF_TEXTREL` into an audited private
marker. Marked executable pages are writable only during guest linker relocation
and constructors; the runtime seals them to the original permissions after
preload or `dlopen` returns. A failed seal fails the load. This permits legacy
text relocations without leaving code pages writable for the process lifetime.

Use `tools/apk_preflight.py --sysroot <arm32-sysroot> --guest-lib-dir
<bundled-arm32-lib-dir> <apk>` for a static dependency report. It resolves the
packaged ARM32 libraries against bundled support libraries, the globally
preloaded `libzbcompat.so`/`libzbjni.so`, and the supplied sysroot. It reports
missing dependencies and ABI mismatches, then reports unresolved
strong imports only when the dependency closure is complete. Weak imports are
inventoried but do not count as missing. Versioned imports and relocation types
are inventoried. Static symbol findings are candidates: lazy `dlopen`, symbol
interposition, version details, and runtime namespace decisions require execution.
Without a supplied sysroot, external dependencies are marked unverified.

Runtime proxy reports label missing library, missing symbol, ABI mismatch and
relocation failures from guest linker errors. A guest SIGSEGV or SIGILL is labeled
as an execution fault separately from loader failures. Unrecognized linker text
is labeled `other` and the original detail is kept.

The Android 17 guest sysroot and the bundled `libzbcompat` shim are the current
API baseline. Target-SDK behavior is passed to the guest linker. Older Android
or vendor libraries are supported only when their dependencies and imported APIs
resolve against these explicit guest files. No general old-API or vendor coverage
is claimed from this baseline.

## Isolated evidence

`loader_dynamic` and `loader_namespace_dynamic` exercise recursive dependencies,
constructor/destructor order, TLS, weak and versioned references, GNU hash,
RELRO, `dladdr`, `dlsym`, `dlerror`, `RTLD_NOW`, `RTLD_LAZY`, `RTLD_LOCAL`,
`RTLD_GLOBAL`, `RTLD_NOLOAD`, `RTLD_NODELETE`, load/unload, missing library,
unresolved strong relocation and the executable namespace boundary. The existing
`cxx_dynamic` fixture exercises ARM32 exception unwinding through `.ARM.exidx`.
`elf_loader_validation_test`, `elf_fixups_test`, `path_translation_test`, and
`runtime_report_test` cover malformed inputs, fixups, sealing and error categories.
These are host/QEMU and static preflight checks, not Android device or user-app
compatibility proof.
