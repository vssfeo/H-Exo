// H-Exo Omni-Core: GICv3 Implementation
// Interrupt controller management for RK3399

#include "gicv3.h"
#include "uart.h"

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
    
    // Enable Group 1
    ctrl |= (1 << 1); // EnableGrp1NS
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
        // SGI/PPI (Redistributor)
        u32 bit = 1 << (irq % 32);
        gicr_write(GICR_ISENABLER0, bit);
    } else {
        // SPI (Distributor)
        u32 reg = GICD_ISENABLER + (irq / 32) * 4;
        u32 bit = 1 << (irq % 32);
        gicd_write(reg, bit);
    }
}
