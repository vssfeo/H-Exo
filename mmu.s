// H-Exo Omni-Core: EL2 MMU Identity Map for RK3399
//
// VA = PA (identity), 4KB granule, T0SZ=32 (4GB), four 1GB L1 block entries:
//   [0] 0x00000000–0x3FFFFFFF  Normal WB Cacheable  (DRAM low 1GB)
//   [1] 0x40000000–0x7FFFFFFF  Normal WB Cacheable  (DRAM high 1GB)
//   [2] 0x80000000–0xBFFFFFFF  Device-nGnRnE        (unused — mapped safe)
//   [3] 0xC0000000–0xFFFFFFFF  Device-nGnRnE        (UART/GIC/GMAC/PMU MMIO)
//
// MAIR_EL2:  Attr0=Device-nGnRnE(0x00)  Attr1=Normal WB(0xFF)
// U-Boot leaves EL2 MMU active — mmu_init() switches TTBR0_EL2 to our table.

// ---- Page table in BSS (zeroed by _start, 4KB-aligned) --------------------
.section .bss
.align 12
.global page_table_l1
page_table_l1:
    .space 4096          // 512 × 8B; only first 4 entries used (4 × 1GB)

// ---- Code ------------------------------------------------------------------
.section .text
.global mmu_init
.global mmu_enable
.global mmu_disable
.global mmu_secondary_el1_enable

// Descriptor bit constants
.equ PTE_BLOCK,      0x1          // [1:0]=01  block entry
.equ PTE_ATTR_DEV,   0x0          // [4:2]=000 AttrIdx=0 (Device-nGnRnE)
.equ PTE_ATTR_NORM,  0x4          // [4:2]=001 AttrIdx=1 (Normal WB)
.equ PTE_SH_IS,      (3 << 8)     // [9:8]=11  Inner Shareable
.equ PTE_AF,         (1 << 10)    // [10]=1    Access Flag (required)
// XN bits live in the upper 16-bit field — assembled via movk
// bits[54:53] = UXN|PXN = 0x0060 in bits[63:48]
.equ PTE_XN_HI,      0x0060       // movk ..., lsl #48

// Normal 1GB block attrs = AF(0x400)|SH_IS(0x300)|AttrIdx1(0x4)|Block(0x1) = 0x705
// Device 1GB block attrs = AF(0x400)|AttrIdx0(0x0)|Block(0x1) = 0x401 + XN upper bits

//===========================================================================
// mmu_init — build L1 table, configure MAIR/TCR/TTBR0_EL2, flush TLB
//===========================================================================
mmu_init:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    // ---- 1. MAIR_EL2 -------------------------------------------------------
    // Attr0 [7:0]  = 0x00  Device-nGnRnE
    // Attr1 [15:8] = 0xFF  Normal Inner/Outer WB WA RA
    mov     x0, #0xFF00          // Attr1=0xFF at byte 1, Attr0=0x00 at byte 0
    msr     mair_el2, x0
    isb

    // ---- 2. L1 page table (BSS, already zeroed by _start) -----------------
    adr     x8, page_table_l1

    // Entry 0: PA=0x00000000  Normal WB  (AttrIdx=1, SH=IS, AF, Block)
    // 0x0000_0000_0000_0705
    mov     x1, #(PTE_BLOCK | PTE_ATTR_NORM | PTE_SH_IS | PTE_AF)
    str     x1, [x8, #0]

    // Entry 1: PA=0x40000000  Normal WB
    // 0x0000_0000_4000_0705
    movk    x1, #0x4000, lsl #16          // insert PA[31:16] = 0x4000
    str     x1, [x8, #8]

    // Entry 2: PA=0x80000000  Device-nGnRnE  (AttrIdx=0, SH=00, AF, Block, XN)
    // 0x0060_0000_8000_0401
    mov     x1, #(PTE_BLOCK | PTE_ATTR_DEV | PTE_AF)
    movk    x1, #0x8000, lsl #16          // PA[31:16]
    movk    x1, #PTE_XN_HI, lsl #48      // UXN|PXN
    str     x1, [x8, #16]

    // Entry 3: PA=0xC0000000  Device-nGnRnE  (UART/GIC/GMAC all here)
    // 0x0060_0000_C000_0401
    mov     x1, #(PTE_BLOCK | PTE_ATTR_DEV | PTE_AF)
    movk    x1, #0xC000, lsl #16
    movk    x1, #PTE_XN_HI, lsl #48
    str     x1, [x8, #24]

    // DSB: ensure table stores reach coherency point before TTBR update
    dsb     sy

    // ---- 3. TCR_EL2 (non-VHE, 32-bit effective) ----------------------------
    // T0SZ=32  IRGN0=01  ORGN0=01  SH0=11  TG0=00(4KB)  PS=010(40-bit PA)
    mov     x0, #32                        // T0SZ=32 → 4GB VA space
    orr     x0, x0, #(1 << 8)             // IRGN0=01  Inner WB WA
    orr     x0, x0, #(1 << 10)            // ORGN0=01  Outer WB WA
    orr     x0, x0, #(3 << 12)            // SH0=11    Inner Shareable
    orr     x0, x0, #(2 << 16)            // PS=010    40-bit PA (safe on RK3399)
    msr     tcr_el2, x0
    isb

    // ---- 4. TTBR0_EL2 → our L1 table (VA=PA identity so adr gives PA) -----
    adr     x0, page_table_l1
    msr     ttbr0_el2, x0
    isb

    // ---- 5. Invalidate all EL2 TLBs (IS = broadcast across Inner Shareable) -
    tlbi    alle2is
    dsb     sy
    isb

    ldp     x29, x30, [sp], #16
    ret

//===========================================================================
// mmu_enable — set M+C+I in SCTLR_EL2 (idempotent; U-Boot already set them)
//===========================================================================
mmu_enable:
    mrs     x0, sctlr_el2
    orr     x0, x0, #(1 << 0)     // M=1  MMU enable
    orr     x0, x0, #(1 << 2)     // C=1  D-cache enable
    orr     x0, x0, #(1 << 12)    // I=1  I-cache enable
    msr     sctlr_el2, x0
    isb
    ret

//===========================================================================
// mmu_disable — clear M bit only (debug / emergency)
//===========================================================================
mmu_disable:
    mrs     x0, sctlr_el2
    bic     x0, x0, #(1 << 0)     // M=0
    msr     sctlr_el2, x0
    dsb     sy
    isb
    ret

//===========================================================================
// mmu_secondary_el1_enable — EL1 MMU + D-cache for secondary cores (EL1)
// Shares the same identity-map page table built by mmu_init() at EL2.
// Must be called after mmu_init() (page_table_l1 populated) and before
// smp_secondary_main().  No arguments.  Clobbers x0, x1.
//===========================================================================
mmu_secondary_el1_enable:
    // 1. MAIR_EL1 — match EL2: Attr0=Device(0x00), Attr1=Normal WB(0xFF)
    mov     x0, #0xFF00
    msr     mair_el1, x0
    isb

    // 2. TCR_EL1 — T0SZ=32 (4GB VA), 4KB granule, WB Inner-Shareable, 40-bit PA
    //    EPD1=1 disables TTBR1 region (we only use low 4GB identity map)
    mov     x0, #32                // T0SZ=32
    orr     x0, x0, #0x100         // IRGN0=01  Inner WB RA WA
    orr     x0, x0, #0x400         // ORGN0=01  Outer WB RA WA
    orr     x0, x0, #0x3000        // SH0=11    Inner Shareable
    orr     x0, x0, #0x200000      // T1SZ=32
    orr     x0, x0, #0x800000      // EPD1=1    disable TTBR1 walks
    orr     x0, x0, #0x1000000     // IRGN1=01
    orr     x0, x0, #0x4000000     // ORGN1=01
    orr     x0, x0, #0x30000000    // SH1=11
    orr     x0, x0, #0x80000000    // TG1=10    4KB
    movk    x0, #0x0002, lsl #32   // IPS=010   40-bit intermediate PA
    msr     tcr_el1, x0
    isb

    // 3. TTBR0_EL1 — same page table as EL2 (VA=PA identity map)
    adr     x0, page_table_l1
    msr     ttbr0_el1, x0
    isb

    // 4. Invalidate EL1 TLBs + I-cache (Inner Shareable broadcast)
    tlbi    vmalle1is
    ic      ialluis
    dsb     ish
    isb

    // 5. Enable MMU (M=1), D-cache (C=1), I-cache (I=1) in SCTLR_EL1
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 0)     // M=1
    orr     x0, x0, #(1 << 2)     // C=1
    orr     x0, x0, #(1 << 12)    // I=1
    msr     sctlr_el1, x0
    isb

    ret
