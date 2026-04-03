// H-Exo Omni-Core: Simplified Boot for Debugging
.section ".text.boot"
.global _start

_start:
    // Core ID check
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary_core
    
    // Secondary cores hang
secondary_hang:
    wfi
    b       secondary_hang

primary_core:
    // Set stack
    ldr     x0, =__stack_top
    mov     sp, x0
    
    // Clear BSS
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x1, x1, x0
    cbz     x1, bss_done
    
    mov     x2, #0
bss_loop:
    str     x2, [x0], #8
    subs    x1, x1, #8
    b.gt    bss_loop
    
bss_done:
    // Initialize MMU
    bl      mmu_init
    bl      mmu_enable
    
    // Jump to C
    bl      kmain
    
primary_hang:
    wfi
    b       primary_hang
