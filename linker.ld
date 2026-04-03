ENTRY(_start)
SECTIONS
{
    /* Для U-Boot raw-load теста используем тот же адрес, куда читаем kernel.bin */
    . = 0x02080000;
    
    /* Exception Vector Table - MUST be 2KB aligned */
    .text.vectors : ALIGN(2K) {
        KEEP(*(.text.vectors))
        . = ALIGN(2K);
    }
    
    /* Code section */
    .text : ALIGN(4K) {
        KEEP(*(.text.boot))
        *(.text*)
        . = ALIGN(4K);
    }
    
    /* Read-only data - CRITICAL: Neural network weights */
    .rodata : ALIGN(64) {
        __rodata_start = .;
        *(.rodata*)
        . = ALIGN(64);
        __rodata_end = .;
    }
    
    /* Initialized data */
    .data : ALIGN(4K) {
        *(.data*)
        . = ALIGN(8);
    }
    
    /* Zero-initialized data */
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    
    /* Stack - OPTIMIZED from 64KB to 16KB */
    . = ALIGN(4K);
    __stack_top = . + 0x4000;  /* 16KB stack instead of 64KB */
}