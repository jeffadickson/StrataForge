/* STRATA-20 QEMU AArch64 `virt` Machine Bare-Metal EL2 Execution Linker Script */
ENTRY(_start_qemu_el2)

MEMORY {
    HYP_CODE   (rx) : ORIGIN = 0x40080000, LENGTH = 0x00080000
    HYP_RAM    (rw) : ORIGIN = 0x40100000, LENGTH = 0x00100000
    MRAM_NVLOG (rw) : ORIGIN = 0x40200000, LENGTH = 0x00010000
}

SECTIONS {
    .text : {
        KEEP(*(.text.boot))
        KEEP(*(.text.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > HYP_CODE

    .data : {
        *(.data*)
    } > HYP_RAM AT > HYP_CODE

    .bss (NOLOAD) : {
        . = ALIGN(8);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > HYP_RAM

    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += 0x8000; /* 32 KB EL2 stack */
        __el2_stack_top = .;
        . = ALIGN(16);
        . += 0x4000; /* 16 KB EL1 guest partition stack */
        __el1_guest_stack_top = .;
    } > HYP_RAM

    .nv_flight_log (NOLOAD) : {
        . = ALIGN(64);
        __nv_flight_log_start = .;
        KEEP(*(.nv_flight_log*))
        __nv_flight_log_end = .;
    } > MRAM_NVLOG
}
