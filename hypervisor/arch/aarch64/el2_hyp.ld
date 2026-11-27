/* STRATA-20 Versal Cortex-A72 Bare-Metal EL2 Hypervisor Linker Script */
ENTRY(_start_el2)

MEMORY {
    HYP_CODE       (rx) : ORIGIN = 0x00100000, LENGTH = 0x00080000
    HYP_RAM        (rw) : ORIGIN = 0x00180000, LENGTH = 0x00080000
    CRITICAL_STORE (rw) : ORIGIN = 0x00200000, LENGTH = 0x00004000
}

SECTIONS {
    .text : {
        KEEP(*(.text.boot))
        KEEP(*(.text.vectors))
        *(.text*)
        *(.rodata*)
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
        . += 0x4000; /* 16 KB static EL2 stack */
        __el2_stack_top = .;
    } > HYP_RAM

    .critical_store (NOLOAD) : {
        __critical_store_start = .;
        KEEP(*(.critical_store*))
        __critical_store_end = .;
    } > CRITICAL_STORE
}
