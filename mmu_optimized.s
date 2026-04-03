// H-Exo Omni-Core: Optimized Memory Management Unit
// Distributed Address Space Foundation with Fine-Grained Control

.section .data
.align 12

// Level 1 Translation Table (512GB coverage)
.global page_table_l1
page_table_l1:
    .space 4096

// Level 2 Translation Tables (for fine-grained 2MB mappings)
.global page_table_l2_ram
page_table_l2_ram:
    .space 4096             // L2 table for first 1GB of RAM

.global page_table_l2_peripherals
page_table_l2_peripherals:
    .space 4096             // L2 table for peripheral region

// Reserved: Future distributed memory mappings
.global remote_memory_tables
remote_memory_tables:
    .space 8192             // Space for remote node memory mappings

.section .text
.global mmu_init
.global mmu_enable
.global mmu_map_remote_region
.global tlb_invalidate_all
.global tlb_invalidate_va

// Memory attributes
.equ MAIR_DEVICE_nGnRnE,  0x00
.equ MAIR_DEVICE_nGnRE,   0x04
.equ MAIR_NORMAL_NC,      0x44      // Normal Non-Cacheable
.equ MAIR_NORMAL_WB,      0xFF      // Normal Write-Back

// Descriptor bits
.equ DESC_VALID,          0x1
.equ DESC_TABLE,          0x3       // Valid + Table
.equ DESC_BLOCK,          0x1       // Valid + Block (bit 1 = 0)
.equ DESC_PAGE,           0x3       // Valid + Page
.equ DESC_AF,             0x400     // Access Flag
.equ DESC_SH_INNER,       0x300     // Inner Shareable
.equ DESC_SH_OUTER,       0x200     // Outer Shareable
.equ DESC_ATTR_IDX0,      0x0       // Device
.equ DESC_ATTR_IDX1,      0x4       // Normal WB
.equ DESC_ATTR_IDX2,      0x8       // Normal NC
.equ DESC_AP_RW_EL1,      0x0       // RW at EL1
.equ DESC_AP_RO_EL1,      0x80      // RO at EL1
.equ DESC_PXN,            0x0020000000000000
.equ DESC_UXN,            0x0040000000000000

//==============================================================================
// mmu_init: Initialize page tables with fine-grained mappings
//==============================================================================
mmu_init:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    stp     x19, x20, [sp, #-16]!

    //--------------------------------------------------------------------------
    // Configure MAIR_EL1 (4 memory types)
    //--------------------------------------------------------------------------
    movz    x0, #0x0000                     // MAIR_DEVICE_nGnRnE at index 0
    movk    x0, #0xFF00, lsl #0             // MAIR_NORMAL_WB at index 1
    movk    x0, #0x4404, lsl #16            // MAIR_NORMAL_NC at index 2, DEVICE_nGnRE at index 3
    msr     mair_el1, x0
    isb

    //--------------------------------------------------------------------------
    // Clear all page tables
    //--------------------------------------------------------------------------
    adr     x0, page_table_l1
    mov     x1, #512
    mov     x2, #0
1:  str     x2, [x0], #8
    subs    x1, x1, #1
    b.ne    1b

    adr     x0, page_table_l2_ram
    mov     x1, #512
2:  str     x2, [x0], #8
    subs    x1, x1, #1
    b.ne    2b

    adr     x0, page_table_l2_peripherals
    mov     x1, #512
3:  str     x2, [x0], #8
    subs    x1, x1, #1
    b.ne    3b

    //--------------------------------------------------------------------------
    // Build L1 Table with L2 pointers for fine-grained control
    //--------------------------------------------------------------------------
    adr     x0, page_table_l1

    // Entry 0: Point to L2 table for RAM (0x00000000-0x3FFFFFFF)
    adr     x1, page_table_l2_ram
    orr     x1, x1, #DESC_TABLE
    str     x1, [x0, #0]

    // Entry 1: Map 0x40000000-0x7FFFFFFF as 1GB block (Normal Cacheable)
    movz    x1, #0x4000, lsl #16
    orr     x1, x1, #DESC_VALID
    orr     x1, x1, #DESC_ATTR_IDX1
    orr     x1, x1, #DESC_AF
    orr     x1, x1, #DESC_SH_INNER
    str     x1, [x0, #8]

    // Entry 3: Point to L2 table for peripherals (0xC0000000-0xFFFFFFFF)
    adr     x1, page_table_l2_peripherals
    orr     x1, x1, #DESC_TABLE
    str     x1, [x0, #24]

    //--------------------------------------------------------------------------
    // Build L2 Table for RAM (2MB blocks for first 1GB)
    //--------------------------------------------------------------------------
    adr     x0, page_table_l2_ram
    mov     x19, #0                 // Physical address counter
    mov     x20, #512               // 512 entries = 1GB

l2_ram_loop:
    mov     x1, x19
    orr     x1, x1, #DESC_VALID
    orr     x1, x1, #DESC_AF
    orr     x1, x1, #DESC_ATTR_IDX1     // Normal WB
    orr     x1, x1, #DESC_SH_INNER
    str     x1, [x0], #8
    
    add     x19, x19, #0x200000     // Next 2MB block
    subs    x20, x20, #1
    b.ne    l2_ram_loop

    //--------------------------------------------------------------------------
    // Build L2 Table for Peripherals (2MB blocks)
    //--------------------------------------------------------------------------
    adr     x0, page_table_l2_peripherals
    movz    x19, #0xC000, lsl #16   // Start at 0xC0000000
    mov     x20, #512

l2_periph_loop:
    mov     x1, x19
    orr     x1, x1, #0x1                // DESC_VALID
    orr     x1, x1, #0x400              // DESC_AF
    orr     x1, x1, #0x200              // DESC_SH_OUTER
    movk    x1, #0x0060, lsl #48        // PXN + UXN
    str     x1, [x0], #8
    
    add     x19, x19, #0x200000
    subs    x20, x20, #1
    b.ne    l2_periph_loop

    //--------------------------------------------------------------------------
    // Configure TCR_EL1 with optimal settings
    //--------------------------------------------------------------------------
    mov     x0, #32                     // T0SZ = 32 (4GB)
    orr     x0, x0, #0x100              // IRGN0 = Inner WB
    orr     x0, x0, #0x400              // ORGN0 = Outer WB
    orr     x0, x0, #0x3000             // SH0 = Inner Shareable
    movk    x0, #0x0002, lsl #32        // IPS = 40-bit PA
    orr     x0, x0, #(1 << 23)          // EPD1 = Disable TTBR1
    orr     x0, x0, #(1 << 37)          // TBI0 = Top Byte Ignore
    msr     tcr_el1, x0
    isb

    //--------------------------------------------------------------------------
    // Set TTBR0_EL1
    //--------------------------------------------------------------------------
    adr     x0, page_table_l1
    msr     ttbr0_el1, x0
    isb

    //--------------------------------------------------------------------------
    // Invalidate TLB
    //--------------------------------------------------------------------------
    bl      tlb_invalidate_all

    ldp     x19, x20, [sp], #16
    ldp     x29, x30, [sp], #16
    ret

//==============================================================================
// mmu_enable: Enable MMU with optimal cache policy
//==============================================================================
mmu_enable:
    // Ensure all previous writes are visible
    dsb     sy
    isb

    mrs     x0, sctlr_el1
    
    // Enable: MMU, D-Cache, I-Cache, Alignment check
    orr     x0, x0, #(1 << 0)           // M: MMU enable
    orr     x0, x0, #(1 << 2)           // C: D-Cache enable
    orr     x0, x0, #(1 << 12)          // I: I-Cache enable
    orr     x0, x0, #(1 << 1)           // A: Alignment check
    
    // Disable: Write XOR Execute, Privileged Access Never
    bic     x0, x0, #(1 << 19)          // WXN: disable
    bic     x0, x0, #(1 << 23)          // SPAN: disable
    
    msr     sctlr_el1, x0
    isb

    ret

//==============================================================================
// mmu_map_remote_region: Map remote node memory (for L2 mesh)
// x0 = remote physical address
// x1 = local virtual address
// x2 = size in bytes
// x3 = attributes (0=device, 1=normal)
//==============================================================================
mmu_map_remote_region:
    // TODO: Implement dynamic mapping for distributed address space
    // This will be used when L2 mesh is active
    ret

//==============================================================================
// TLB Management Functions
//==============================================================================
tlb_invalidate_all:
    dsb     sy
    tlbi    vmalle1                     // Invalidate all TLB entries
    dsb     sy
    isb
    ret

tlb_invalidate_va:
    // x0 = virtual address to invalidate
    lsr     x0, x0, #12                 // Convert to page number
    tlbi    vaae1, x0                   // Invalidate by VA
    dsb     sy
    isb
    ret

//==============================================================================
// Cache Management Functions
//==============================================================================
.global dcache_clean_invalidate_all
dcache_clean_invalidate_all:
    mrs     x0, clidr_el1
    and     x3, x0, #0x7000000
    lsr     x3, x3, #23                 // Cache level value

    mov     x10, #0                     // Start at L1
cache_loop:
    add     x2, x10, x10, lsr #1
    lsr     x1, x0, x2
    and     x1, x1, #7
    cmp     x1, #2
    b.lt    cache_skip

    msr     csselr_el1, x10
    isb
    mrs     x1, ccsidr_el1
    and     x2, x1, #7
    add     x2, x2, #4
    mov     x4, #0x3ff
    and     x4, x4, x1, lsr #3
    clz     w5, w4
    mov     x7, #0x7fff
    and     x7, x7, x1, lsr #13

way_loop:
    mov     x9, x4
set_loop:
    lsl     x6, x9, x5
    orr     x11, x10, x6
    lsl     x6, x7, x2
    orr     x11, x11, x6
    dc      cisw, x11
    subs    x9, x9, #1
    b.ge    set_loop
    subs    x7, x7, #1
    b.ge    way_loop

cache_skip:
    add     x10, x10, #2
    cmp     x3, x10
    b.gt    cache_loop

    dsb     sy
    isb
    ret

.global icache_invalidate_all
icache_invalidate_all:
    ic      iallu
    dsb     sy
    isb
    ret
