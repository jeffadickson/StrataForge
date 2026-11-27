// =============================================================================
// STRATA-20 QEMU AArch64 `virt` Bare-Metal EL2 Boot, Vector Table & EL1 ERET Bridge
// Executes live on `qemu-system-aarch64 -machine virt,virtualization=on -cpu cortex-a72`.
// =============================================================================
    .section .text.boot, "ax"
    .global _start_qemu_el2
    .type   _start_qemu_el2, %function

_start_qemu_el2:
    msr     daifset, #0xF

    // Set up 32 KB static EL2 stack
    ldr     x0, =__el2_stack_top
    mov     sp, x0

    // Install 2 KB-aligned EL2 exception vector table
    ldr     x0, =qemu_el2_vectors
    msr     vbar_el2, x0
    isb

    // Zero .bss section deterministically (preserves .nv_flight_log)
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:
    cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    // Run static C++20 global constructors (.init_array) deterministically
    ldr     x19, =__init_array_start
    ldr     x20, =__init_array_end
3:
    cmp     x19, x20
    b.hs    4f
    ldr     x0, [x19], #8
    blr     x0
    b       3b
4:
    bl      strata_qemu_el2_main

    // Exit QEMU via AArch64 semihosting SYS_EXIT (exit code in w0)
    b       qemu_semihosting_exit

// -----------------------------------------------------------------------------
// void qemu_semihosting_exit(int exit_code)
// -----------------------------------------------------------------------------
    .global qemu_semihosting_exit
    .type   qemu_semihosting_exit, %function
qemu_semihosting_exit:
    // Parameter block on stack: [0] = ADP_Stopped_ApplicationExit (0x20026), [1] = exit_code
    sub     sp, sp, #16
    movz    x2, #0x0026
    movk    x2, #0x0002, lsl #16
    uxtw    x3, w0
    stp     x2, x3, [sp]
    mov     w0, #0x18       // SYS_EXIT
    mov     x1, sp
    hlt     #0xF000
1:
    wfi
    b       1b

// -----------------------------------------------------------------------------
// uint64_t qemu_run_el1_guest(uint64_t entry_pc, uint64_t guest_sp)
// Saves EL2 callee-saved context, ERETs to EL1h guest partition, and returns
// the trapped ESR_EL2 syndrome when the guest triggers a fatal Stage-2 abort.
// -----------------------------------------------------------------------------
    .global qemu_run_el1_guest
    .type   qemu_run_el1_guest, %function
qemu_run_el1_guest:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]

    // Save EL2 resume SP so fatal guest trap handler can unwind back to EL2 caller
    ldr     x2, =g_qemu_el2_resume_sp
    mov     x3, sp
    str     x3, [x2]

    // Configure EL1 guest stack, entry PC, and SPSR_EL2 (EL1h = 0x5, DAIF masked = 0x3C0)
    msr     sp_el1, x1
    msr     elr_el2, x0
    mov     x2, #0x3C5
    msr     spsr_el2, x2
    isb
    eret

// Unwind target jumped to from el2_lower_a64_sync when guest suffers fatal fault (x0 = esr_el2)
el2_unwind_from_faulted_guest:
    ldr     x2, =g_qemu_el2_resume_sp
    ldr     x3, [x2]
    mov     sp, x3
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret

// -----------------------------------------------------------------------------
// AArch64 EL2 Vector Table (2 KB aligned)
// -----------------------------------------------------------------------------
    .section .text.vectors, "ax"
    .balign 2048
    .global qemu_el2_vectors

.macro QVEC target
    .balign 128
    b       \target
.endm

qemu_el2_vectors:
    // Current EL with SP0
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    // Current EL with SPx
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    // Lower EL using AArch64 (EL1 Guest -> EL2 HVC / Stage-2 Data Abort)
    QVEC    qemu_el2_lower_a64_sync
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    // Lower EL using AArch32
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled
    QVEC    qemu_el2_unhandled

qemu_el2_lower_a64_sync:
    sub     sp, sp, #80
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    str     x30,    [sp, #64]

    mrs     x0, esr_el2
    mrs     x1, elr_el2
    mrs     x2, far_el2
    mrs     x3, hpfar_el2
    mov     x4, sp
    bl      strata_qemu_sync_handler

    // Return value: 0 = resume EL1 guest via ERET; non-zero = abort guest & unwind with ESR in x0
    cbnz    x0, el2_unwind_from_faulted_guest

    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldr     x30,    [sp, #64]
    add     sp, sp, #80
    eret

qemu_el2_unhandled:
    mov     w0, #99
    b       qemu_semihosting_exit

    .section .bss
    .balign 8
    .global g_qemu_el2_resume_sp
g_qemu_el2_resume_sp:
    .quad   0
