# Third-party code

## dynarmic

- Source: https://github.com/Vita3K/dynarmic (git submodule `third_party/dynarmic`)
- Pinned commit: `86458a0bd369d63ba4c2ef812cacbb6c9080c065` ("fix compilation with Clang 20")
- License: 0BSD
- Local patches: `third_party/patches/dynarmic-*.patch`, applied on top of the pin.

Apply after cloning or updating the submodule:

```
git submodule update --init; git -C third_party/dynarmic apply ../patches/dynarmic-0001-thumb32-armv8.patch
```

### dynarmic-0001-thumb32-armv8.patch

Adds ARMv8 AArch32 T32 instructions that the A32 decoder already had.

1. The "load acquire" / "store release" family:
`LDAB`, `LDAH`, `LDAEX`, `LDAEXB`, `LDAEXH`, `LDAEXD`, `STLB`, `STLH`, `STLEX`, `STLEXB`,
`STLEXH`, `STLEXD`. The A32 (ARM-mode) decoder already had all of them, and T32 had only
`LDA` and `STL`.

- **Semantics.** `LDA*`/`STL*` are plain loads/stores; `LDAEX*`/`STLEX*` behave like
  `LDREX*`/`STREX*`.
- **Encodings.** Taken from the NDK r29 assembler (`clang --target=armv8a`, then
  `llvm-objdump --triple=thumbv8a`), for example `ldab r1,[r3]` = `e8d3 1f8f`,
  `ldaexh r1,[r3]` = `e8d3 1fdf`, `stlexd r4,r1,r2,[r3]` = `e8c3 12f4`.
2. `CRC32{B,H,W}` and `CRC32C{B,H,W}` (`FAC n F d 10zz m` / `FAD n F d 10zz m`), reusing the
   A32 `CRC32Variant`. scudo in the GSI libc computes chunk checksums with `crc32cw`
   unconditionally, so every `malloc` needs it.

3. **Precise memory aborts in A32 on arm64.** `A32AddressSpace::GenerateIR` now skips
   `A32GetSetElimination` when `check_halt_on_memory_access` is set, as
   `A64AddressSpace` already did. Without it, a halted block leaves guest registers
   written by earlier instructions of the block uncommitted.

- **Why the family is needed.** The GSI's arm32 bionic is built for armv8-a and uses these
  instructions heavily (the linker alone has ~1000 of them).
  `__libc_arc4random_ready()` (LDAB) and `pthread_mutex_lock()` (LDAEXH) run during
  linker startup, so without this patch every dynamic guest dies with an undefined
  instruction.

Other ARMv8 instructions found in the sysroot and their Dynarmic status:
- VFPv8 (`VSEL`, `VRINT*`, `VCVT{A,N,P,M}`, `VMAXNM`/`VMINNM`) and AES/SHA256: supported.
- SHA1 is decoded as UNDEFINED; it is only used when `AT_HWCAP2` advertises it, and zbrun
  reports `AT_HWCAP2 = 0`.
- T32 `HLT` is missing; it appears only on fatal-error paths.
