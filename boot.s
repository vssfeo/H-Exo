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

.macro DEBUG_PUTC_X reg
    movz    x13, #0xFF1A, lsl #16
1:  ldr     w14, [x13, #UART_USR]
    tst     w14, #2
    b.eq    1b
    str     w\reg, [x13, #UART_THR]
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
    // 1. Check CPU ID first. Do NOT touch EL2 sysregs before we know the core
    // is actually in EL2; some secondaries may arrive in EL1.
    mrs     x0, mpidr_el1
    and     x2, x0, #0xFFFFFF
    cbz     x2, master

    // ===== SECONDARY CPU REACHED _start =====
    // Write to MULTIPLE PMU GRF OS_REGs for redundancy.
    // OS_REG2 (0xFF320308) = fixed 0xBB (proves _start reached regardless of OS_REG1)
    // OS_REG1 (0xFF320304) = Aff0 | 0xA0 (identifies WHICH core)
    // OS_REG3 (0xFF32030C) = CurrentEL (verifies EL)
    movz    x22, #0xFF32, lsl #16   // PMUGRF base high
    movk    x22, #0x0300            // x22 = 0xFF320300 (OS_REG0 base)
    mov     w24, #0xBB
    str     w24, [x22, #0x08]       // OS_REG2 = 0xBB (fixed canary)
    and     w23, w0, #0xFF          // w23 = Aff0 (1..5)
    add     w23, w23, #0xA0         // w23 = 0xA1..0xA5
    str     w23, [x22, #0x04]       // OS_REG1 = Aff0 | 0xA0
    mrs     x25, CurrentEL
    lsr     w25, w25, #2
    str     w25, [x22, #0x0C]       // OS_REG3 = EL (1 or 2)

    // Install minimal exception vector only when running at EL2.
    // Avoids UNDEF on EL1 secondaries (msr vbar_el2 at EL1).
    cmp     w25, #2
    b.ne    0f
    adr     x9, _sec_exc_vector
    msr     vbar_el2, x9
    isb
0:

    // UART-BLAST 'T': direct MMIO write, NO busy-wait.
    movz    x20, #0xFF1A, lsl #16
    mov     w21, #'T'
    str     w21, [x20, #0x00]

    // TOMBSTONE (BSS)
    ldr     x1, =smp_start_tombstone
    and     x3, x0, #0xFFFFFF
    str     x3, [x1]
    // Trace page
    adrp    x4, smp_trace_page
    add     x4, x4, :lo12:smp_trace_page
    mov     x5, #1
    lsl     x5, x5, x3
    ldr     x6, [x4, #8]
    orr     x6, x6, x5
    str     x6, [x4, #8]
    str     x3, [x4, #64]
    // TRAMPOLINE → secondary_entry
    ubfx    x1, x2, #8, #8
    and     x0, x2, #0xFF
    lsl     x1, x1, #2
    add     x0, x0, x1
    ldr     x3, =secondary_entry
    br      x3

master:
    // Primary-only: initialize UART2 clocks before any debug UART access.
    // U-Boot already has the port working in practice, but keep this here for
    // cold boot safety on the boot CPU only.
    INIT_UART2_CLOCK

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

    // 4. Zero BSS section (not included in .bin, may contain TFTP-buffer garbage)
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
bss_zero:
    cmp     x0, x1
    b.ge    bss_done
    str     xzr, [x0], #8
    b       bss_zero
bss_done:

    // 5. Jump to C code (MMU enabled from U-Boot)
    bl      kmain

hang:
    // DIAGNOSTIC: prove a core reached hang (secondary core sent to _start?)
    movz    x0, #0xFF1A, lsl #16
9:  ldr     w1, [x0, #0x7C]     // UART_USR
    tst     w1, #2
    b.eq    9b
    mov     w1, #'H'
    str     w1, [x0, #0x00]     // THR
    wfi
    b       hang

// Secondary core entry point - called by PSCI CPU_ON from smp_init()
// BL31 branches here (or to _start trampoline) after PSCI CPU_ON.
// On RK3399 Armbian BL31 v1.1, secondary enters at EL2 (same as primary).
//
// DIAGNOSTIC PROTOCOL: Write beacon at 0x02000000 AS FIRST THING,
// before any BSS access, UART, or stack setup. This proves whether
// secondary_entry is reached at all. beacon[0]=core_idx, beacon[1]=0xBEEFDEAD,
// beacon[2]=CurrentEL, beacon[3]=MPIDR_EL1 lower 32 bits.
.global secondary_entry
.align 12
secondary_entry:
    // --- STEP 1: Beacon write — absolute address, zero BSS dependencies ---
    movz    x11, #0x0200, lsl #16   // x11 = 0x02000000 (confirmed NS DRAM)
    mrs     x0,  mpidr_el1
    ubfx    x1,  x0, #8, #8         // x1 = Aff1
    and     x0,  x0, #0xFF          // x0 = Aff0
    lsl     x1,  x1, #2             // x1 = Aff1*4
    add     x0,  x0, x1             // x0 = sequential_idx (A53:0-3, A72:4-5)
    str     x0,  [x11]              // beacon[0] = core_idx
    mov     x12, #0xDEAD
    movk    x12, #0xBEEF, lsl #16
    str     x12, [x11, #8]          // beacon[1] = 0xBEEFDEAD sentinel
    mrs     x10, CurrentEL          // EL diagnostic (no exception, always legal)
    lsr     x10, x10, #2            // x10 = EL (2=EL2, 1=EL1)
    str     x10, [x11, #16]         // beacon[2] = CurrentEL
    mrs     x9,  mpidr_el1
    str     x9,  [x11, #24]         // beacon[3] = raw MPIDR_EL1
    // dsb is intentionally omitted — non-cached writes bypass cache,
    // hit DRAM directly; core 0 does civac before psci_cpu_on so it reads DRAM.

    // UART-BLAST 'S': direct MMIO write, NO busy-wait.
    // If 'S' appears on UART → secondary_entry was reached.
    // This is independent of beacon DRAM write (MMIO vs DRAM).
    movz    x20, #0xFF1A, lsl #16
    mov     w21, #'S'
    str     w21, [x20, #0x00]       // blast 'S' to THR directly

    // --- STEP 2: Trace page (BSS) — may show zeros on core 0 due to cache
    //   staleness, but does not crash secondary. Low priority vs beacon. ---
    adrp    x6, smp_trace_page
    add     x6, x6, :lo12:smp_trace_page
    mov     x7, #1
    lsl     x7, x7, x0              // bit for this core
    ldr     x16, [x6, #16]          // SMP_TRACE_SEC_SEEN_MASK (may read stale 0)
    orr     x16, x16, x7
    str     x16, [x6, #16]
    str     x0,  [x6, #64]          // SMP_TRACE_LAST
    adrp    x15, smp_entry_stage
    add     x15, x15, :lo12:smp_entry_stage
    lsl     x14, x0, #3
    add     x15, x15, x14
    mov     x13, #0x11
    str     x13, [x15]              // stage=0x11: reached secondary_entry

    // --- STEP 3: UART 's' — no busy-wait risk from MMIO being inaccessible ---
    // Secondary is at EL2 (NS); UART2 is an NS peripheral, accessible.
    mov     x8, #'s'
    DEBUG_PUTC_X 8

    // --- STEP 4: idle counter beacon (smp_idle_counters) ---
    adrp    x3, smp_idle_counters
    add     x3, x3, :lo12:smp_idle_counters
    lsl     x4, x0, #3
    add     x3, x3, x4
    add     x4, x0, #1
    str     x4, [x3]

    // --- STEP 5: Stack setup (per-core 4KB slot below __stack_top) ---
    ldr     x1, =__stack_top
    mov     x2, #0x1000
    add     x9, x0, #1
    mul     x2, x2, x9
    sub     x1, x1, x2
    mov     sp, x1
    ldr     x12, [x6, #40]          // SMP_TRACE_STACK_MASK
    orr     x12, x12, x7
    str     x12, [x6, #40]
    mov     x12, sp
    str     x12, [x6, #80]          // last observed SP

    // --- STEP 6: EL check — skip EL2 registers if at EL1 ---
    str     x10, [x6, #72]          // record EL (x10 = CurrentEL from step 1)
    ldr     x12, [x6, #32]          // SMP_TRACE_EL_MASK
    orr     x12, x12, x7
    str     x12, [x6, #32]
    mov     x8, #'m'
    DEBUG_PUTC_X 8
    cmp     x10, #1
    b.ne    .Lskip_el1_mmu
    bl      mmu_secondary_el1_enable
.Lskip_el1_mmu:
    ldr     x12, [x6, #48]          // SMP_TRACE_MMU_MASK
    orr     x12, x12, x7
    str     x12, [x6, #48]
    adrp    x15, smp_entry_stage
    add     x15, x15, :lo12:smp_entry_stage
    lsl     x14, x0, #3
    add     x15, x15, x14
    mov     x13, #0x22
    str     x13, [x15]
    mov     x8, #'M'
    DEBUG_PUTC_X 8

    // --- STEP 7: C worker ---
    adrp    x15, smp_entry_stage
    add     x15, x15, :lo12:smp_entry_stage
    lsl     x14, x0, #3
    add     x15, x15, x14
    mov     x13, #0x33
    str     x13, [x15]
    ldr     x12, [x6, #56]          // SMP_TRACE_WQ_MASK
    orr     x12, x12, x7
    str     x12, [x6, #56]
    str     x0,  [x6, #64]
    mov     x8, #'C'
    DEBUG_PUTC_X 8
    bl      smp_secondary_main
    mov     x8, #'R'
    DEBUG_PUTC_X 8
3:  wfe
    b       3b

// Minimal A72 probe entrypoint:
// - no stack
// - no MMU assumptions
// - only writes distinct beacon signature then parks in WFE
// Used to prove whether EL3 actually transfers control to our NS entry.
.global secondary_entry_probe
.align 7
secondary_entry_probe:
    mrs     x0,  mpidr_el1
    ubfx    x1,  x0, #8, #8         // Aff1
    and     x0,  x0, #0xFF          // Aff0
    lsl     x1,  x1, #2
    add     x0,  x0, x1             // linear core idx
    movz    x11, #0x0200, lsl #16   // beacon base 0x02000000
    str     x0,  [x11]              // beacon[0] = idx
    movz    x12, #0xA72A, lsl #16
    movk    x12, #0x72A7
    str     x12, [x11, #8]          // beacon[1] = 0xA72A72A7
    mrs     x10, CurrentEL
    lsr     x10, x10, #2
    str     x10, [x11, #16]         // beacon[2] = EL
    mrs     x9,  mpidr_el1
    str     x9,  [x11, #24]         // beacon[3] = MPIDR
    movz    x20, #0xFF1A, lsl #16
    mov     w21, #'P'
    str     w21, [x20, #0x00]       // UART 'P' probe mark
1:  wfe
    b       1b

// Secondary-only minimal exception vector table.
// If secondary takes ANY exception before reaching secondary_entry's proper
// vector setup, this handler writes 0xEE to PMUGRF OS_REG2 to signal a fault.
// Must be 2KB aligned (VBAR requirement).
.align 11
_sec_exc_vector:
    // Synchronous from current EL with SP0 (offset 0x000)
    movz    x10, #0xFF32, lsl #16
    movk    x10, #0x0308            // OS_REG2
    mov     w11, #0xEE
    str     w11, [x10]
    mrs     x12, esr_el2
    movz    x10, #0xFF32, lsl #16
    movk    x10, #0x030C            // OS_REG3
    str     w12, [x10]              // ESR_EL2 syndrome → OS_REG3
4:  wfe
    b       4b
.align 7
    // IRQ from current EL with SP0 (offset 0x080)
    b       4b
.align 7
    // FIQ from current EL with SP0 (offset 0x100)
    b       4b
.align 7
    // SError from current EL with SP0 (offset 0x180)
    b       4b
.align 7
    // Synchronous from current EL with SPx (offset 0x200)
    movz    x10, #0xFF32, lsl #16
    movk    x10, #0x0308
    mov     w11, #0xEF
    str     w11, [x10]
    mrs     x12, esr_el2
    movz    x10, #0xFF32, lsl #16
    movk    x10, #0x030C
    str     w12, [x10]
5:  wfe
    b       5b
.align 7
    // IRQ from current EL with SPx (offset 0x280)
    b       5b
.align 7
    // FIQ from current EL with SPx (offset 0x300)
    b       5b
.align 7
    // SError from current EL with SPx (offset 0x380)
    b       5b
.align 7
    // Synchronous from lower EL AArch64 (offset 0x400)
    b       5b
.align 7
    b       5b
.align 7
    b       5b
.align 7
    b       5b
.align 7
    // Synchronous from lower EL AArch32 (offset 0x600)
    b       5b
.align 7
    b       5b
.align 7
    b       5b
.align 7
    b       5b

// ============================================================
// Relay trampoline + EL2 exception vector — deployed to 0x200000.
//
// Layout within one 4KB page (smp_trampoline is .align 12):
//   +0x000  Relay code  (< 0x80 bytes)
//   +0x800  EL2 exception vector table (vbar_el2 = 0x200800)
//            16 entries × 128 bytes = 2048 bytes
//   +0x1000 smp_trampoline_end
//
// DIAGNOSTIC PLAN:
//   Step A  GRF OS_REG2=0xBB, OS_REG1=Aff0|0xA0, OS_REG3=EL
//           ↳ proves trampoline was reached
//   Step B  Store 0xCAFEBABE to beacon[1] = 0x02000008
//           ↳ proves DRAM writes work from secondary at 0x200000
//   Step C  msr vbar_el2, #0x200800  (exception vector inside trampoline page)
//           ↳ any fault after this → handler writes 0xEE to OS_REG2, ESR to OS_REG3
//   Step D  br to 0x02081000 (secondary_entry linked address)
//           ↳ if instruction-fetch aborts → EC in ESR exposed via GRF OS_REG3
//           ↳ if it succeeds but first store aborts → same, data-abort EC
//           ↳ if everything works → BEACON HIT + 'S' on UART
// ============================================================
.global smp_trampoline
.global smp_trampoline_end
.align 12
smp_trampoline:
    // --- Step A: GRF canaries ---
    movz    x0, #0xFF32, lsl #16
    movk    x0, #0x0308
    mov     w1, #0xBB
    str     w1, [x0]
    dsb     sy
    movz    x0, #0xFF32, lsl #16
    movk    x0, #0x0304
    mrs     x2, mpidr_el1
    and     w1, w2, #0xFF              // Aff0 (bits 7:0)
    ubfx    x3, x2, #8, #4            // Aff1[3:0] from bits [11:8]
    orr     w1, w1, w3, lsl #4        // combine: Aff0 | (Aff1<<4)
    add     w1, w1, #0xA0             // 0xA0=A53-core-0, 0xB0=A72-core-0, etc.
    str     w1, [x0]
    dsb     sy
    // OS_REG3 = DAIF raw value (A-bit=0x100 means SError UNMASKED=catastrophe)
    movz    x0, #0xFF32, lsl #16
    movk    x0, #0x030C
    mrs     x3, daif
    str     w3, [x0]
    dsb     sy
    movz    x4, #0xFF1A, lsl #16
    mov     w5, #0x58           // UART 'X'
    str     w5, [x4]

    // --- Step B: DRAM beacon test (0x02000008 = beacon[1]) ---
    movz    x7, #0x0200, lsl #16
    movz    x8, #0xCAFE, lsl #16
    movk    x8, #0xBABE         // x8 = 0xCAFEBABE
    str     x8, [x7, #8]        // beacon[1] = 0xCAFEBABE
    dsb     sy

    // --- Step C: Install EL2 exception vector at trampoline+0x800 = 0x200800 ---
    movz    x9, #0x0020, lsl #16
    movk    x9, #0x0800         // x9 = 0x00200800
    msr     vbar_el2, x9
    isb

    // --- Step D: Enable EL2 MMU (M+C+I) using core 0's page tables ---
    // ROOT CAUSE: non-coherent AXI instruction fetch (caches/MMU off) returns
    // SLVERR for 0x02081000; A53 inserts POISON → sync EC=0 fault.
    // SCTLR.I=1 alone didn't help: without M=1 the memory type remains
    // Normal-NonCacheable, so fetch still goes non-coherent direct-to-DRAM.
    // Fix: enable the SAME identity-map MMU core 0 already set up. With M=1
    // the fetch is Normal-Cacheable, goes through L2/CCI (coherent), SLVERR gone.
    //
    // Core 0 stored its EL2 MMU registers to beacon[4..6] (0x02000020+):
    //   beacon[4] @ 0x02000020 = TTBR0_EL2
    //   beacon[5] @ 0x02000028 = TCR_EL2
    //   beacon[6] @ 0x02000030 = MAIR_EL2
    movz    x9,  #0x0200, lsl #16    // x9 = 0x02000000 (beacon)
    ldr     x10, [x9, #32]           // TTBR0_EL2 (beacon[4])
    ldr     x11, [x9, #40]           // TCR_EL2   (beacon[5])
    ldr     x12, [x9, #48]           // MAIR_EL2  (beacon[6])
    msr     ttbr0_el2, x10
    msr     tcr_el2,   x11
    msr     mair_el2,  x12
    isb
    // Enable MMU + D-cache + I-cache in SCTLR_EL2
    mrs     x9,  sctlr_el2
    orr     x9,  x9,  #(1 << 0)     // M=1: MMU enable
    orr     x9,  x9,  #(1 << 2)     // C=1: data cache
    orr     x9,  x9,  #(1 << 12)    // I=1: instruction cache
    msr     sctlr_el2, x9
    isb

    // Mask SError/IRQ/FIQ/debug — belt+suspenders for any residual async fault.
    msr     daifset, #0xF
    isb

    // --- Step E: Absolute branch to secondary_entry = 0x02081000 ---
    // With MMU + caches on, fetch of 0x02081000 is Normal-Cacheable, coherent.
    // Identity map: VA 0x02081000 = PA 0x02081000.
    movz    x6, #0x0208, lsl #16
    movk    x6, #0x1000
    br      x6

// ============================================================
// EL2 exception vector at trampoline+0x800 (deployed: 0x200800).
// ALL 16 entries run the same handler.
// On any EL2 fault: GRF OS_REG2←0xEE, OS_REG3←ESR_EL2, UART←'E', loop.
// ============================================================
.align 11   // advance to next 2048-byte boundary = smp_trampoline + 0x800
    .rept   16
    movz    x10, #0xFF32, lsl #16
    movk    x10, #0x0308
    mov     w11, #0xEE
    str     w11, [x10]          // OS_REG2 = 0xEE (overrides 0xBB)
    mrs     x12, esr_el2
    movz    x10, #0xFF32, lsl #16
    movk    x10, #0x030C
    str     w12, [x10]          // OS_REG3 = ESR_EL2
    movz    x10, #0xFF1A, lsl #16
    mov     w11, #0x45          // UART 'E'
    str     w11, [x10]
9:  wfe
    b       9b
    .align  7   // pad each entry to 128 bytes
    .endr
smp_trampoline_end:

