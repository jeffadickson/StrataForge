// =============================================================================
// STRATA-20 Versal Cortex-A72 EL2 Exception Vector Table & Boot Entry
// Implements STRATA-HYP-001, STRATA-HYP-008, STRATA-HYP-009.
// 2 KB aligned VBAR_EL2 table trapping EL1 HVC hypercalls & synchronous faults
// into C++20 `strata_el2_sync_handler` with zero heap and zero libc dependency.
// =============================================================================
    .section .text.boot, "ax"
    .global _start_el2
    .type   _start_el2, %function

_start_el2:
    // Mask DAIF interrupts during EL2 initialization
    msr     daifset, #0xF

    // Set up static EL2 hypervisor stack pointer
    ldr     x0, =__el2_stack_top
    mov     sp, x0

    // Install 2 KB-aligned EL2 vector table
    ldr     x0, =strata_el2_vectors
    msr     vbar_el2, x0
    isb

    // Zero .bss section deterministically
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
    // Enter C++20 bare-metal hypervisor entry point
    bl      strata_el2_main

5:
    wfi
    b       5b

// -----------------------------------------------------------------------------
// AArch64 EL2 Vector Table (16 entries x 128 bytes = 2048 bytes, 2 KB aligned)
// -----------------------------------------------------------------------------
    .section .text.vectors, "ax"
    .balign 2048
    .global strata_el2_vectors

.macro VECTOR_STUB target
    .balign 128
    b       \target
.endm

strata_el2_vectors:
    // Current EL with SP0
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    // Current EL with SPx
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    // Lower EL using AArch64 (Guest Partition EL1 -> EL2 HVC / Data/Instr Abort)
    VECTOR_STUB el2_lower_a64_sync
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    // Lower EL using AArch32
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap
    VECTOR_STUB el2_unhandled_trap

// -----------------------------------------------------------------------------
// Lower EL AArch64 Synchronous Exception / HVC Hypercall Dispatch
// -----------------------------------------------------------------------------
el2_lower_a64_sync:
    // Save volatile guest registers x0..x7 + x30 (LR) on EL2 stack
    sub     sp, sp, #80
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    str     x30,    [sp, #64]

    // x0 = ESR_EL2, x1 = ELR_EL2, x2 = FAR_EL2, x3 = saved_regs_ptr
    mrs     x0, esr_el2
    mrs     x1, elr_el2
    mrs     x2, far_el2
    mov     x3, sp
    bl      strata_el2_sync_handler

    // Return value in x0 is written back into guest x0 slot
    str     x0, [sp, #0]

    // Restore guest registers and return to EL1 partition
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldr     x30,    [sp, #64]
    add     sp, sp, #80
    eret

el2_unhandled_trap:
    wfe
    b       el2_unhandled_trap
