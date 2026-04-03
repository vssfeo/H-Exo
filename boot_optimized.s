// H-Exo Omni-Core: Optimized Boot Sequence
// Phase 0: Aleph Engine - Hardware Hijacking & Multi-Core Foundation
// Architecture: ARMv8-A AArch64 for RK3399 (Cortex-A72 + A53)

.section ".text.boot"
.global _start
.global secondary_cores_wake

// Exception Level constants
.equ EL0, 0
.equ EL1, 1
.equ EL2, 2
.equ EL3, 3

// Core-specific stack sizes (16KB per core)
.equ STACK_SIZE, 0x4000

// UART2 registers for emergency debug output
.equ UART2_BASE, 0xFF1A0000
.equ UART_THR, 0x00
.equ UART_USR, 0x7C

// CRU (Clock & Reset Unit) registers for UART2 clock gating
.equ CRU_BASE, 0xFF760000
.equ CRU_CLKGATE_CON16, 0x240       // UART2 clock gate control
.equ CRU_CLKGATE_CON34, 0x288       // PMU bus clock gate

// PMU registers for power domain
.equ PMU_BASE, 0xFF310000
.equ PMU_BUS_IDLE_ST, 0x64

//==============================================================================
// CRU Clock Initialization - Enable UART2 clocks
// CRITICAL: Must be called before any UART access to avoid Data Abort
// Clobbers: x0, x1, x2
//==============================================================================
.macro INIT_UART2_CLOCKS
    // Enable UART2 clock in CRU_CLKGATE_CON16
    // Bit [15:0] = write mask (1 = allow write), Bit [31:16] = gate control (0 = enable)
    movz    x0, #0x7600, lsl #16    // CRU_BASE = 0xFF760000
    movk    x0, #0xFF00, lsl #32
    
    // Write to CRU_CLKGATE_CON16: enable UART2 clock (bit 5)
    // Write mask = 0x0020 (bit 5), gate = 0x0000 (enable)
    mov     w1, #0x00200000         // Mask bit 5, clear gate bit 5
    str     w1, [x0, #CRU_CLKGATE_CON16]
    
    // Small delay for clock stabilization
    mov     x2, #100
1:  sub     x2, x2, #1
    cbnz    x2, 1b
.endm

//==============================================================================
// Emergency UART Macro - Direct hardware access, NO STACK, NO DEPENDENCIES
// CRITICAL: Can be called before stack initialization
// WARNING: Clobbers x0, x1 - caller must save if needed
// PREREQUISITE: INIT_UART2_CLOCKS must be called first
//==============================================================================
.macro DEBUG_PUTC char
    movz    x0, #0x1A00, lsl #16    // UART2_BASE = 0xFF1A0000
    movk    x0, #0xFF00, lsl #32
1:  ldr     w1, [x0, #UART_USR]     // Read USR (Status Register)
    tst     w1, #2                  // Check TFNF (Transmit FIFO Not Full)
    b.eq    1b                      // Wait if busy
    mov     w1, #\char              // Load character
    str     w1, [x0, #UART_THR]     // Write to THR
.endm

_start:
    // CRITICAL: Initialize UART2 clocks BEFORE first beacon
    INIT_UART2_CLOCKS
    
    // BEACON 1: Hardware under control
    DEBUG_PUTC '1'
    //==========================================================================
    // Stage 1: Exception Level Detection & Transition
    //==========================================================================
    mrs     x0, CurrentEL
    lsr     x0, x0, #2              // Extract EL bits [3:2]
    cmp     x0, #EL3
    b.eq    from_el3
    cmp     x0, #EL2
    b.eq    from_el2
    // Already at EL1 or EL0
    b       el1_entry

from_el3:
    // BEACON 2: About to transition EL3 -> EL2
    DEBUG_PUTC '2'
    
    // Configure EL3 -> EL2 transition
    mov     x0, #0x5b1              // RES1 bits + NS bit
    msr     scr_el3, x0
    
    mov     x0, #0x3c9              // EL2h (use SP_EL2)
    msr     spsr_el3, x0
    
    adr     x0, from_el2
    msr     elr_el3, x0
    eret                            // Drop to EL2

from_el2:
    // BEACON 3: Successfully transitioned to EL2
    DEBUG_PUTC '3'
    
    // Configure EL2 -> EL1 transition
    mov     x0, #(1 << 31)          // RES1 bit
    msr     hcr_el2, x0
    
    mov     x0, #0x3c5              // EL1h (use SP_EL1)
    msr     spsr_el2, x0
    
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret                            // Drop to EL1

el1_entry:
    // BEACON 4: Now at EL1, about to initialize MMU
    DEBUG_PUTC '4'
    //==========================================================================
    // Stage 2: CPU Feature Detection
    //==========================================================================
    mrs     x0, id_aa64isar0_el1
    // Check for AES support (bits [7:4])
    ubfx    x1, x0, #4, #4
    cbnz    x1, 1f
    // No crypto extensions - note this for later
1:
    
    //==========================================================================
    // Stage 3: Multi-Core Identification
    //==========================================================================
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF           // Extract Affinity Level 0 (core ID)
    cbz     x0, primary_core        // Core 0 = primary
    
    //==========================================================================
    // Secondary Core Path (Cores 1-5)
    //==========================================================================
secondary_core:
    // Each secondary core gets its own stack
    // Stack layout: primary at __stack_top, then -16KB per core
    ldr     x1, =__stack_top
    mov     x2, #STACK_SIZE
    mul     x2, x0, x2              // core_id * STACK_SIZE
    sub     x1, x1, x2              // Adjust stack pointer
    mov     sp, x1
    
    // Secondary cores wait for wake signal
    adr     x1, secondary_cores_wake
1:  ldr     x2, [x1]
    cbz     x2, 1b                  // Spin until wake signal
    
    // TODO: Jump to secondary_main when implemented
    b       secondary_hang

secondary_hang:
    wfi
    b       secondary_hang

    //==========================================================================
    // Primary Core Path (Core 0)
    //==========================================================================
primary_core:
    // Set up primary core stack
    ldr     x0, =__stack_top
    mov     sp, x0
    
    //==========================================================================
    // Stage 4: Clear BSS Section
    //==========================================================================
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x1, x1, x0              // BSS size
    cbz     x1, bss_done            // Skip if no BSS
    
    mov     x2, #0
bss_loop:
    str     x2, [x0], #8
    subs    x1, x1, #8
    b.gt    bss_loop
    
bss_done:
    //==========================================================================
    // Stage 5: Install Exception Vector Table
    //==========================================================================
    ldr     x0, =vector_table
    msr     vbar_el1, x0
    isb
    
    //==========================================================================
    // Stage 6: Initialize Hardware RNG (for crypto-addressing foundation)
    //==========================================================================
    // RK3399 has hardware RNG at 0xFF8B8000
    // Initialize it for future crypto operations
    bl      rng_init
    
    //==========================================================================
    // Stage 7: Memory Dominance (MMU + Caches)
    //==========================================================================
    bl      mmu_init
    bl      mmu_enable
    
    // BEACON 5: MMU enabled successfully, page tables working
    DEBUG_PUTC '5'
    
    //==========================================================================
    // Stage 8: Enter C Kernel
    //==========================================================================
    bl      kmain
    
    // Should never return, but if it does:
primary_hang:
    wfi
    b       primary_hang

//==============================================================================
// Hardware RNG Initialization (for crypto-addressing)
//==============================================================================
.equ RNG_BASE, 0xFF8B8000

rng_init:
    // Enable RNG and start generating entropy
    movz    x0, #0x8B80, lsl #16    // RNG_BASE = 0xFF8B8000
    movk    x0, #0xFF00, lsl #32
    mov     x1, #0x01               // Enable bit
    str     w1, [x0, #0x400]        // RNG_CTRL
    ret

//==============================================================================
// Exception Vector Table (EL1)
// 16 entries: 4 exception types × 4 sources (Current EL SP0/SPx, Lower EL AArch64/32)
//==============================================================================
.align 11                           // Vector table must be 2KB aligned
vector_table:
    // Current EL with SP0
    .align 7
    b       sync_exception_sp0
    .align 7
    b       irq_exception_sp0
    .align 7
    b       fiq_exception_sp0
    .align 7
    b       serror_exception_sp0
    
    // Current EL with SPx
    .align 7
    b       sync_exception_spx
    .align 7
    b       irq_exception_spx
    .align 7
    b       fiq_exception_spx
    .align 7
    b       serror_exception_spx
    
    // Lower EL (AArch64)
    .align 7
    b       sync_exception_lower64
    .align 7
    b       irq_exception_lower64
    .align 7
    b       fiq_exception_lower64
    .align 7
    b       serror_exception_lower64
    
    // Lower EL (AArch32)
    .align 7
    b       sync_exception_lower32
    .align 7
    b       irq_exception_lower32
    .align 7
    b       fiq_exception_lower32
    .align 7
    b       serror_exception_lower32

//==============================================================================
// Exception Handlers (Stubs - will be expanded later)
//==============================================================================
sync_exception_sp0:
sync_exception_spx:
sync_exception_lower64:
sync_exception_lower32:
    // Save all registers to stack
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!
    stp     x4, x5, [sp, #-16]!
    stp     x6, x7, [sp, #-16]!
    stp     x8, x9, [sp, #-16]!
    stp     x10, x11, [sp, #-16]!
    stp     x12, x13, [sp, #-16]!
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    stp     x18, x19, [sp, #-16]!
    stp     x20, x21, [sp, #-16]!
    stp     x22, x23, [sp, #-16]!
    stp     x24, x25, [sp, #-16]!
    stp     x26, x27, [sp, #-16]!
    stp     x28, x29, [sp, #-16]!
    str     x30, [sp, #-16]!
    
    // Call C exception handler
    bl      handle_sync_exception
    
    // Restore registers
    ldr     x30, [sp], #16
    ldp     x28, x29, [sp], #16
    ldp     x26, x27, [sp], #16
    ldp     x24, x25, [sp], #16
    ldp     x22, x23, [sp], #16
    ldp     x20, x21, [sp], #16
    ldp     x18, x19, [sp], #16
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
    ldp     x12, x13, [sp], #16
    ldp     x10, x11, [sp], #16
    ldp     x8, x9, [sp], #16
    ldp     x6, x7, [sp], #16
    ldp     x4, x5, [sp], #16
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
    eret

irq_exception_sp0:
irq_exception_spx:
irq_exception_lower64:
irq_exception_lower32:
    // TODO: Implement IRQ handling for GIC-400
    b       exception_hang

fiq_exception_sp0:
fiq_exception_spx:
fiq_exception_lower64:
fiq_exception_lower32:
    // TODO: Implement FIQ handling
    b       exception_hang

serror_exception_sp0:
serror_exception_spx:
serror_exception_lower64:
serror_exception_lower32:
    // System Error - critical
    b       exception_hang

exception_hang:
    wfi
    b       exception_hang

//==============================================================================
// Data Section
//==============================================================================
.section .data
.align 3

// Wake signal for secondary cores (0 = sleep, 1 = wake)
secondary_cores_wake:
    .quad   0

// Reserved region for future L2 mesh node identity
.global node_identity
node_identity:
    .space  64              // 512-bit identity (hardware-backed public key hash)

// Reserved region for distributed address space metadata
.global distributed_memory_map
distributed_memory_map:
    .space  256             // Metadata for remote memory regions
