// H-Exo Omni-Core: Memory Dominance Module
// AArch64 MMU Initialization for RK3399
// Phase 1: Establish foundation for distributed address space

.section .data
.align 12  // Page tables must be 4KB aligned

// Level 1 Translation Table (covers 512GB, we use first few entries)
.global page_table_l1
page_table_l1:
    .space 4096  // 512 entries x 8 bytes

.section .text
.global mmu_init
.global mmu_enable

// Memory Attribute Indirection Register values
// Index 0: Device-nGnRnE (non-Gathering, non-Reordering, no Early Write Ack)
// Index 1: Normal Memory, Inner/Outer Write-Back Cacheable
.equ MAIR_DEVICE_nGnRnE,  0x00
.equ MAIR_NORMAL_WB,      0xFF

// Page table descriptor bits
.equ PTE_VALID,           0x1        // (1 << 0)
.equ PTE_TABLE,           0x2        // (1 << 1)
.equ PTE_BLOCK,           0x0        // (0 << 1)
.equ PTE_AF,              0x400      // (1 << 10) Access Flag
.equ PTE_SH_INNER,        0x300      // (3 << 8) Inner Shareable
.equ PTE_SH_OUTER,        0x200      // (2 << 8) Outer Shareable

// Memory attributes (index into MAIR)
.equ PTE_ATTR_DEVICE,     0x0        // (0 << 2) MAIR index 0
.equ PTE_ATTR_NORMAL,     0x4        // (1 << 2) MAIR index 1

// Access permissions
.equ PTE_AP_RW_EL1,       0x0        // (0 << 6) Read/Write at EL1
.equ PTE_UXN,             0x0040000000000000  // (1 << 54) Unprivileged Execute Never
.equ PTE_PXN,             0x0020000000000000  // (1 << 53) Privileged Execute Never

//==============================================================================
// mmu_init: Initialize page tables and MMU configuration registers
// This function sets up the foundation for H-Exo's distributed address space
//==============================================================================
mmu_init:
    // Save link register
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    //--------------------------------------------------------------------------
    // Step 1: Configure MAIR_EL1 (Memory Attribute Indirection Register)
    //--------------------------------------------------------------------------
    // MAIR_EL1[7:0]   = 0x00 (Device-nGnRnE)
    // MAIR_EL1[15:8]  = 0xFF (Normal, Inner/Outer Write-Back Cacheable)
    mov     x0, #MAIR_DEVICE_nGnRnE
    orr     x0, x0, #(MAIR_NORMAL_WB << 8)
    msr     mair_el1, x0
    isb

    //--------------------------------------------------------------------------
    // Step 2: Build Level 1 Page Table
    //--------------------------------------------------------------------------
    adr     x0, page_table_l1
    
    // Clear entire L1 table
    mov     x1, #512              // 512 entries
    mov     x2, #0
1:  str     x2, [x0], #8
    subs    x1, x1, #1
    b.ne    1b

    // Reset pointer to start of table
    adr     x0, page_table_l1

    //--------------------------------------------------------------------------
    // Entry 0: Map 0x00000000 - 0x3FFFFFFF (1GB) as Normal Cacheable (RAM)
    // Descriptor = PA | Valid(1) | AF(0x400) | AttrIdx1(4) | SH_Inner(0x300)
    //--------------------------------------------------------------------------
    mov     x1, #0x1                           // Valid bit
    orr     x1, x1, #0x4                       // AttrIdx = 1 (Normal memory)
    orr     x1, x1, #(3 << 8)                  // SH = Inner Shareable
    orr     x1, x1, #(1 << 10)                 // AF = Access Flag
    str     x1, [x0, #0]                       // Store at index 0

    //--------------------------------------------------------------------------
    // Entry 1: Map 0x40000000 - 0x7FFFFFFF (1GB) as Normal Cacheable (RAM)
    //--------------------------------------------------------------------------
    movz    x1, #0x4000, lsl #16               // 0x40000000
    orr     x1, x1, #0x1                       // Valid
    orr     x1, x1, #0x4                       // AttrIdx = 1
    orr     x1, x1, #(3 << 8)                  // SH = Inner
    orr     x1, x1, #(1 << 10)                 // AF
    str     x1, [x0, #8]                       // Store at index 1

    //--------------------------------------------------------------------------
    // Entry 3: Map 0xC0000000 - 0xFFFFFFFF (1GB) as Device Memory (Peripherals)
    // This covers the RK3399 peripheral region at 0xFF000000
    // Descriptor = PA | Valid(1) | AF(0x400) | AttrIdx0(0) | SH_Outer(0x200)
    //--------------------------------------------------------------------------
    movz    x1, #0xC000, lsl #16               // 0xC0000000
    orr     x1, x1, #0x1                       // Valid
    orr     x1, x1, #(2 << 8)                  // SH = Outer Shareable
    orr     x1, x1, #(1 << 10)                 // AF
    movk    x1, #0x0060, lsl #48               // PXN(bit53) + UXN(bit54)
    str     x1, [x0, #24]                      // Store at index 3

    //--------------------------------------------------------------------------
    // Step 3: Configure TCR_EL1 (Translation Control Register)
    //--------------------------------------------------------------------------
    // TCR_EL1 configuration:
    // - T0SZ = 32 (2^(64-32) = 4GB address space for TTBR0)
    // - IRGN0 = 0b01 (Inner Write-Back Cacheable)
    // - ORGN0 = 0b01 (Outer Write-Back Cacheable)
    // - SH0 = 0b11 (Inner Shareable)
    // - TG0 = 0b00 (4KB granule)
    // - IPS = 0b010 (40-bit physical address, 1TB)
    
    mov     x0, #32                            // T0SZ = 32 (4GB VA space)
    orr     x0, x0, #0x100                     // IRGN0 = 0b01 << 8 (Inner WB)
    orr     x0, x0, #0x400                     // ORGN0 = 0b01 << 10 (Outer WB)
    orr     x0, x0, #0x3000                    // SH0 = 0b11 << 12 (Inner Shareable)
    // TG0 = 0 (4KB granule) - already zero, no need to set
    movk    x0, #0x0002, lsl #32               // IPS = 0b010 << 32 (40-bit PA)
    msr     tcr_el1, x0
    isb

    //--------------------------------------------------------------------------
    // Step 4: Set TTBR0_EL1 to point to our page table
    //--------------------------------------------------------------------------
    adr     x0, page_table_l1
    msr     ttbr0_el1, x0
    isb

    //--------------------------------------------------------------------------
    // Step 5: Invalidate TLB and caches
    //--------------------------------------------------------------------------
    tlbi    vmalle1                            // Invalidate all TLB entries
    dsb     sy                                 // Data Synchronization Barrier
    isb                                        // Instruction Synchronization Barrier

    // Restore and return
    ldp     x29, x30, [sp], #16
    ret

//==============================================================================
// mmu_enable: Enable MMU and caches
// This activates the distributed address space
//==============================================================================
mmu_enable:
    // Read current SCTLR_EL1
    mrs     x0, sctlr_el1

    // Enable MMU, caches, and alignment checking
    orr     x0, x0, #(1 << 0)                  // M bit: Enable MMU
    orr     x0, x0, #(1 << 2)                  // C bit: Enable D-cache
    orr     x0, x0, #(1 << 12)                 // I bit: Enable I-cache
    orr     x0, x0, #(1 << 1)                  // A bit: Enable alignment check

    // Write back to SCTLR_EL1
    msr     sctlr_el1, x0
    isb                                        // Ensure MMU is enabled before continuing

    ret

//==============================================================================
// mmu_disable: Disable MMU (for debugging)
//==============================================================================
.global mmu_disable
mmu_disable:
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)                  // Clear M bit
    msr     sctlr_el1, x0
    isb
    ret
