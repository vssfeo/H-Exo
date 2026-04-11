#include "cci.h"

// RK3399 CCI-500 base: MMIO_BASE(0xF8000000) + 0x07B00000 = 0xFFB00000
#define CCI_BASE       0xFFB00000UL

// CCI-500 global control register (offset 0x0)
#define CCI_CTRL       0x0000UL
// CCI-500 status register (offset 0xC) — CHANGE_PENDING_BIT = bit 0
#define CCI_STATUS     0x000CUL
#define CCI_CHANGE_PENDING  0x1UL

// Per TF-A rk3399_def.h: PLAT_RK_CCI_CLUSTER0_SL_IFACE_IX=0 (A53), _CLUSTER1_=1 (A72)
// CCI-500 slave interface offsets: slave N is at base + 0x1000*(N+1)
// Slave 0 (A53 LITTLE cluster ACE) = 0x1000
// Slave 1 (A72 BIG   cluster ACE) = 0x2000
#define CCI_SNOOP_CTRL  0x0000UL   // Snoop Control Register offset within slave page
#define CCI_SLAVE_A53  0x1000UL    // Slave interface 0 = A53 cluster
#define CCI_SLAVE_A72  0x2000UL    // Slave interface 1 = A72 cluster

static inline void cci_write(u64 reg_off, u32 val) {
    *(volatile u32 *)(uintptr_t)(CCI_BASE + reg_off) = val;
}

static inline u32 cci_read(u64 reg_off) {
    return *(volatile u32 *)(uintptr_t)(CCI_BASE + reg_off);
}

result_t cci500_enable(void) {
    // Enable snoop + DVM on both cluster slave interfaces.
    // Writes 0x3 = DVM_EN_BIT(1) | SNOOP_EN_BIT(0) to Snoop Control Register.
    cci_write(CCI_SLAVE_A53 + CCI_SNOOP_CTRL, 0x3);
    asm volatile("dsb ish" ::: "memory");
    // Wait for snoop change to complete on A53 slave before touching A72
    for (u32 t = 0; t < 10000; t++) {
        if (!(cci_read(CCI_STATUS) & CCI_CHANGE_PENDING)) break;
        asm volatile("yield");
    }

    cci_write(CCI_SLAVE_A72 + CCI_SNOOP_CTRL, 0x3);
    asm volatile("dsb ish" ::: "memory");
    for (u32 t = 0; t < 10000; t++) {
        if (!(cci_read(CCI_STATUS) & CCI_CHANGE_PENDING)) break;
        asm volatile("yield");
    }

    // Enable CCI global interconnect.
    cci_write(CCI_CTRL, 0x1);
    asm volatile("dsb sy\n isb" ::: "memory");

    // Wait until enabled bit is visible.
    for (u32 t = 0; t < 1000000; t++) {
        if (cci_read(CCI_CTRL) & 0x1) {
            return OK;
        }
        asm volatile("yield");
    }
    return ERR_TIMEOUT;
}
