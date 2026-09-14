// Native method thunks (AArch64). Each of the 16384 thunks is 8 bytes and loads its own address
// into x16 before jumping to the common entry, which derives the slot number from it. No code
// is generated at run time. The count must match kNativeThunkCount in zb/native_thunks.h.

    .text
    .balign 4
    .globl zb_native_thunk_base
    .hidden zb_native_thunk_base
zb_native_thunk_base:
    .rept 16384
    adr x16, .
    b zb_native_common
    .endr

// Saves x0-x7, d0-d7 and the caller's stack argument pointer as zb::NativeRegs (144 bytes),
// calls zb_native_dispatch(slot, regs), then returns regs.x[0] in x0 and regs.d[0] in d0.
    .globl zb_native_common
    .hidden zb_native_common
zb_native_common:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #144
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp d0, d1, [sp, #64]
    stp d2, d3, [sp, #80]
    stp d4, d5, [sp, #96]
    stp d6, d7, [sp, #112]
    add x9, x29, #16            // caller's stack arguments start at the entry sp
    str x9, [sp, #128]
    adr x10, zb_native_thunk_base
    sub x0, x16, x10
    lsr x0, x0, #3              // slot = (thunk address - base) / 8
    mov x1, sp
    bl zb_native_dispatch
    ldr x0, [sp, #0]
    ldr d0, [sp, #64]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret

    .section .note.GNU-stack,"",%progbits
