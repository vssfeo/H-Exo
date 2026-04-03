ENTRY(_start)
SECTIONS
{
    /* Start address for U-Boot raw-load */
    . = 0x02080000;
    
    /* CRITICAL: Boot code MUST be first at 0x02080000 */
    .text.boot : {
        KEEP(*(.text.boot))
    }
    
    /* Exception Vector Table - placed AFTER boot code */
    .text.vectors : ALIGN(2K) {
        KEEP(*(.text.vectors))
        . = ALIGN(2K);
    }
    
    /* Rest of code */
    .text : ALIGN(4K) {
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
