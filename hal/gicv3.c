// H-Exo Omni-Core: GICv3 Implementation
// Interrupt controller management for RK3399

#include "gicv3.h"

// Register access helpers - proper 64-bit address handling
static inline void gicd_write(u32 reg, u32 val) {
    *(volatile u32*)((uintptr_t)GICD_BASE + reg) = val;
    asm volatile("dmb sy" ::: "memory");
}

static inline u32 gicd_read(u32 reg) {
    u32 val = *(volatile u32*)((uintptr_t)GICD_BASE + reg);
    asm volatile("dmb sy" ::: "memory");
    return val;
}

static inline void gicr_write(u32 reg, u32 val) {
    *(volatile u32*)((uintptr_t)GICR_BASE + reg) = val;
    asm volatile("dmb sy" ::: "memory");
}

static inline u32 gicr_read(u32 reg) {
    u32 val = *(volatile u32*)((uintptr_t)GICR_BASE + reg);
    asm volatile("dmb sy" ::: "memory");
    return val;
}

// Wake all redistributors (clear ProcessorSleep) BEFORE PSCI CPU_ON.
// BL31 v1.3 parks secondary cores in WFI and sets ProcessorSleep=1 for them.
// If ProcessorSleep=1 when BL31 sends the wake SGI during CPU_ON, the SGI is
// silently dropped by the redistributor and the core stays in WFI forever.
// Call this ONCE before smp_init() so the redistributors are ready.
void gicv3_prewake_redistributors(void) {
    // GICR stride = 0x20000 (LPI frame 64KB + SGI frame 64KB per core)
    // RK3399 has 6 CPU interfaces total, so prewake all redistributors.
    for (u32 cpu = 0; cpu < 6; cpu++) {
        volatile u32 *waker = (volatile u32*)(
            (uintptr_t)GICR_BASE + (uintptr_t)cpu * 0x20000 + GICR_WAKER);
        u32 w = *waker;
        w &= ~(1u << 1);  // clear ProcessorSleep
        *waker = w;
        // Wait for ChildrenAsleep to clear (redistributor confirmed awake)
        int t = 1000000;
        while ((*waker & (1u << 2)) && t--) asm volatile("yield");
    }
    asm volatile("dsb sy\n isb" ::: "memory");
}

result_t gicv3_init(void) {
    // 1. Disable Distributor
    gicd_write(GICD_CTLR, 0);
    
    // 2. Wake up Redistributor
    u32 waker = gicr_read(GICR_WAKER);
    waker &= ~(1 << 1); // Clear ProcessorSleep
    gicr_write(GICR_WAKER, waker);
    
    // Wait for ChildrenAsleep to be cleared
    int timeout = 1000000;
    while ((gicr_read(GICR_WAKER) & (1 << 2)) && timeout--) {
        asm volatile("yield");
    }
    
    if (timeout <= 0) return ERR_TIMEOUT;

    // 3. Configure Group 1 (Normal World) interrupts
    // Set ARE bits (Affinity Routing Enable)
    u32 ctrl = gicd_read(GICD_CTLR);
    ctrl |= (1 << 4) | (1 << 5); // ARE_S and ARE_NS
    gicd_write(GICD_CTLR, ctrl);
    
    // Enable Group 0 (BL31 EL3 SGIs) AND Group 1 NS (our IRQs)
    // EnableGrp0 MUST be restored: a pending BL31 Group-0 wake-SGI is blocked
    // until this bit is set again.  Leaving it at 0 silently drops the SGI.
    ctrl |= (1 << 0) | (1 << 1); // EnableGrp0 + EnableGrp1NS
    gicd_write(GICD_CTLR, ctrl);

    // 4. Configure CPU Interface (System Registers)
    u32 sre;
    asm volatile("mrs %0, ICC_SRE_EL2" : "=r"(sre));
    sre |= (1 << 0) | (1 << 1) | (1 << 2); // SRE, DFB, DFE
    asm volatile("msr ICC_SRE_EL2, %0" :: "r"(sre));
    asm volatile("isb");

    // Set priority mask to allow all interrupts
    u32 pmr = 0xFF;
    asm volatile("msr ICC_PMR_EL1, %0" :: "r"((u64)pmr));
    
    // Enable Group 1 interrupts at CPU interface
    u32 igrp = 1;
    asm volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"((u64)igrp));
    asm volatile("isb");

    return OK;
}

void gicv3_enable_irq(u32 irq) {
    if (irq < 32) {
        u32 bit = 1 << (irq % 32);
        gicr_write(GICR_ISENABLER0, bit);
    } else {
        u32 reg = GICD_ISENABLER + (irq / 32) * 4;
        u32 bit = 1 << (irq % 32);
        gicd_write(reg, bit);
    }
}

// Route SPI irq to the CPU described by affinity (0 = core 0, matches MPIDR).
// GICD_IROUTER[n] is a 64-bit register at offset 0x6000 + n*8.
void gicv3_route_irq(u32 irq, u64 affinity) {
    if (irq < 32) return;
    volatile u64* r = (volatile u64*)((uintptr_t)GICD_BASE + GICD_IROUTER + irq * 8);
    *r = affinity;
    asm volatile("dmb sy" ::: "memory");
}

// Set interrupt priority (0 = highest, 0xFF = lowest).
// GICD_IPRIORITYR: one byte per interrupt, packed 4-per-word.
void gicv3_set_priority(u32 irq, u8 prio) {
    u32 reg = GICD_IPRIORITYR + irq;
    u32 shift = (irq & 3) * 8;
    u32 val = gicd_read(reg & ~3u);
    val = (val & ~(0xFFu << shift)) | ((u32)prio << shift);
    gicd_write(reg & ~3u, val);
}

// Acknowledge interrupt: read INTID from ICC_IAR1_EL1 (also deactivates spurious).
u32 gicv3_ack_irq(void) {
    u64 intid;
    asm volatile("mrs %0, ICC_IAR1_EL1" : "=r"(intid));
    asm volatile("isb");
    return (u32)intid;
}

// End-of-interrupt: signal completion to GIC CPU interface.
void gicv3_eoi_irq(u32 intid) {
    asm volatile("msr ICC_EOIR1_EL1, %0" :: "r"((u64)intid));
    asm volatile("isb");
}
