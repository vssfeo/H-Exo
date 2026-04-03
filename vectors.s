// H-Exo Omni-Core: Exception Vector Table (AArch64)
// Preserves system state and redirects to C handlers

.section ".text.vectors"
.align 11 // Vector table must be aligned to 2KB (2^11)

.global exception_vector_table

exception_vector_table:
    // ------------------------------------------------------------------------
    // Current EL with SP_EL0
    // ------------------------------------------------------------------------
    .align 7
curr_el_sp0_sync:     b   exception_handler_sync
    .align 7
curr_el_sp0_irq:      b   exception_handler_irq
    .align 7
curr_el_sp0_fiq:      b   exception_handler_fiq
    .align 7
curr_el_sp0_serror:   b   exception_handler_serror

    // ------------------------------------------------------------------------
    // Current EL with SP_ELx
    // ------------------------------------------------------------------------
    .align 7
curr_el_spx_sync:     b   exception_handler_sync
    .align 7
curr_el_spx_irq:      b   exception_handler_irq
    .align 7
curr_el_spx_fiq:      b   exception_handler_fiq
    .align 7
curr_el_spx_serror:   b   exception_handler_serror

    // ------------------------------------------------------------------------
    // Lower EL using AArch64
    // ------------------------------------------------------------------------
    .align 7
lower_el_a64_sync:    b   exception_handler_sync
    .align 7
lower_el_a64_irq:     b   exception_handler_irq
    .align 7
lower_el_a64_fiq:     b   exception_handler_fiq
    .align 7
lower_el_a64_serror:  b   exception_handler_serror

    // ------------------------------------------------------------------------
    // Lower EL using AArch32
    // ------------------------------------------------------------------------
    .align 7
lower_el_a32_sync:    b   exception_handler_sync
    .align 7
lower_el_a32_irq:     b   exception_handler_irq
    .align 7
lower_el_a32_fiq:     b   exception_handler_fiq
    .align 7
lower_el_a32_serror:  b   exception_handler_serror

// Exception context save/restore macros
// Layout: x0-x30 (31 regs * 8 = 248 bytes) + padding to 256 bytes
.macro SAVE_CONTEXT
    sub     sp, sp, #256
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
.endm

.macro RESTORE_CONTEXT
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #256
    eret
.endm

exception_handler_sync:
    SAVE_CONTEXT
    mov     x0, sp
    bl      handle_sync_exception
    RESTORE_CONTEXT

exception_handler_irq:
    SAVE_CONTEXT
    mov     x0, sp
    bl      handle_irq_exception
    RESTORE_CONTEXT

exception_handler_fiq:
    SAVE_CONTEXT
    mov     x0, sp
    bl      handle_fiq_exception
    RESTORE_CONTEXT

exception_handler_serror:
    SAVE_CONTEXT
    mov     x0, sp
    bl      handle_serror_exception
    RESTORE_CONTEXT
