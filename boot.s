.section ".text.boot"
.global _start

// CRU Clock Gate Registers for UART2
.equ CRU_BASE,          0xFF760000
.equ CRU_CLKGATE_CON_16, (CRU_BASE + 0x1C0)  // UART2 clock gate

// Emergency UART debug (no stack, no dependencies)
.equ UART2_BASE, 0xFF1A0000
.equ UART_THR,   0x00
.equ UART_USR,   0x7C

.macro DEBUG_PUTC char
    movz    x0, #0xFF1A, lsl #16
1:  ldr     w1, [x0, #UART_USR]
    tst     w1, #2
    b.eq    1b
    mov     w1, #\char
    str     w1, [x0, #UART_THR]
.endm

.macro INIT_UART2_CLOCK
    // Enable UART2 clock gate (bit 10 of CRU_CLKGATE_CON_16)
    // CRU_CLKGATE_CON_16 = 0xFF760000 + 0x1C0
    movz    x0, #0xFF76, lsl #16
    ldr     w1, [x0, #0x1C0]     // Load CRU_CLKGATE_CON_16
    bic     w1, w1, #0x400       // Clear bit 10 (UART2 clock gate)
    str     w1, [x0, #0x1C0]     // Store back
.endm

_start:
    // CRITICAL: Initialize UART2 clocks BEFORE any UART access
    // This prevents Synchronous Abort on first DEBUG_PUTC
    INIT_UART2_CLOCK
    
    // 1. Check CPU ID (only core 0 continues)
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, master
    b       hang

master:
    // 2. Determine Exception Level and configure PMU accordingly
    mrs     x1, CurrentEL
    and     x1, x1, #0xC
    cmp     x1, #0x8            // EL2?
    b.eq    pmu_el2_init
    
    // EL1 PMU initialization
pmu_el1_init:
    // Enable user access to PMU (PMUSERENR_EL0)
    mov     x0, #0xF            // Enable all PMU access for EL0
    msr     pmuserenr_el0, x0
    
    // Enable PMU (PMCR_EL0)
    mov     x0, #0x7            // Enable + Reset counters
    msr     pmcr_el0, x0
    
    // Enable cycle counter (PMCNTENSET_EL0)
    mov     x0, #0x80000000     // Bit 31: cycle counter
    msr     pmcntenset_el0, x0
    isb
    b       pmu_done
    
pmu_el2_init:
    // EL2 PMU initialization - CRITICAL FIX
    // MDCR_EL2: Monitor Debug Configuration Register (EL2)
    mrs     x0, mdcr_el2
    bic     x0, x0, #(1 << 11)  // HCCD=0: Don't disable cycle counter in EL2
    bic     x0, x0, #(1 << 7)   // HPME=0: Don't trap event counter access
    orr     x0, x0, #0x1F       // HPMN=31: All event counters available
    msr     mdcr_el2, x0
    isb
    
    // PMUSERENR_EL0: Enable user-mode access to PMU
    mov     x0, #0xF            // EN=1, SW=1, CR=1, ER=1
    msr     pmuserenr_el0, x0
    isb
    
    // PMCR_EL0: Performance Monitors Control Register
    // CRITICAL: Don't reset counter (no bit 2)!
    mrs     x0, pmcr_el0
    orr     x0, x0, #0x1        // E=1: Enable all counters
    orr     x0, x0, #0x40       // LC=1: 64-bit cycle counter
    msr     pmcr_el0, x0
    isb
    
    // PMCNTENSET_EL0: Enable cycle counter
    mov     x0, #0x80000000     // Bit 31: CCNTR enable
    msr     pmcntenset_el0, x0
    isb
    
pmu_done:
    // 2.5 Register Exception Vector Table
    ldr     x0, =exception_vector_table
    msr     vbar_el2, x0
    isb
    
    // 3. Setup stack
    ldr     x0, =__stack_top
    mov     sp, x0
    
    // 4. Jump to C code (MMU enabled from U-Boot)
    bl      kmain

hang:
    wfi                       // Wait For Interrupt
    b       hang

// Обязательно оставь пустую строку ниже этого комментария
