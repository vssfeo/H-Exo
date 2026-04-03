/* H-Exo Omni-Core: Optimized Linker Script
 * Memory layout optimized for distributed computing and future growth
 */

ENTRY(_start)

MEMORY
{
    /* RK3399 Memory Map:
     * 0x00000000 - 0x7FFFFFFF: 2GB DDR3 RAM
     * 0xFF000000 - 0xFFFFFFFF: Peripherals
     * 
     * Kernel placement: 0x02080000 (32MB offset)
     * This leaves room for:
     * - 0x00000000-0x02000000: Reserved for bootloader/DMA buffers
     * - 0x02000000-0x02080000: Page tables and metadata
     */
    RAM (rwx) : ORIGIN = 0x02080000, LENGTH = 16M
}

SECTIONS
{
    /* Code section - read-only, executable */
    .text : ALIGN(4K) {
        __text_start = .;
        *(.text.boot)           /* Boot code first */
        *(.text*)               /* All other code */
        . = ALIGN(4K);
        __text_end = .;
    } > RAM

    /* Exception vector table - must be 2KB aligned */
    .vectors : ALIGN(2K) {
        __vectors_start = .;
        KEEP(*(.vectors))
        . = ALIGN(2K);
        __vectors_end = .;
    } > RAM

    /* Read-only data - CRITICAL: Neural network weights stored here */
    .rodata : ALIGN(4K) {
        __rodata_start = .;
        *(.rodata*)             /* Neural Arbitrator weights */
        . = ALIGN(64);          /* Cache line alignment */
        __rodata_end = .;
    } > RAM

    /* Initialized data section */
    .data : ALIGN(4K) {
        __data_start = .;
        *(.data*)
        . = ALIGN(8);
        __data_end = .;
    } > RAM

    /* Page tables - 4KB aligned */
    .page_tables : ALIGN(4K) {
        __page_tables_start = .;
        *(.page_tables)
        KEEP(*(.data.page_table*))
        . = ALIGN(4K);
        __page_tables_end = .;
    } > RAM

    /* BSS section - zero-initialized data */
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    /* Reserved: Distributed memory metadata */
    .distributed_mem : ALIGN(4K) {
        __distributed_mem_start = .;
        KEEP(*(.data.distributed_memory_map))
        KEEP(*(.data.remote_memory_tables))
        . = ALIGN(4K);
        __distributed_mem_end = .;
    } > RAM

    /* Reserved: Node identity and crypto data */
    .crypto : ALIGN(64) {
        __crypto_start = .;
        KEEP(*(.data.node_identity))
        . = ALIGN(64);
        __crypto_end = .;
    } > RAM

    /* Stack section - grows downward */
    .stack : ALIGN(4K) {
        __stack_bottom = .;
        . += 0x40000;           /* 256KB total stack space */
        __stack_top = .;
        
        /* Per-core stacks (16KB each for 6 cores) */
        __core0_stack_top = .;
        . += 0x4000;
        __core1_stack_top = .;
        . += 0x4000;
        __core2_stack_top = .;
        . += 0x4000;
        __core3_stack_top = .;
        . += 0x4000;
        __core4_stack_top = .;
        . += 0x4000;
        __core5_stack_top = .;
    } > RAM

    /* Heap section - for future dynamic allocation */
    .heap : ALIGN(4K) {
        __heap_start = .;
        . += 4M;                /* 4MB heap */
        __heap_end = .;
    } > RAM

    /* DMA buffers - uncached region for L2 mesh */
    .dma_buffers : ALIGN(4K) {
        __dma_buffers_start = .;
        . += 1M;                /* 1MB DMA buffer pool */
        __dma_buffers_end = .;
    } > RAM

    /* End of kernel image */
    __kernel_end = .;
    
    /* Discard sections we don't need */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

/* Symbols for memory management */
__kernel_size = __kernel_end - __text_start;
__bss_size = __bss_end - __bss_start;
__total_stack_size = __core5_stack_top - __stack_bottom;
