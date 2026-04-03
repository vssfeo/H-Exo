// H-Exo Omni-Core: GMAC "Hello World"
// Minimal driver to initialize PHY and send first frame

#include "gmac.h"
#include "uart.h"

static inline void gmac_write(u32 reg, u32 val) {
    *(volatile u32*)((uintptr_t)GMAC_BASE + reg) = val;
    asm volatile("dmb sy" ::: "memory");
}

static inline u32 gmac_read(u32 reg) {
    u32 val = *(volatile u32*)((uintptr_t)GMAC_BASE + reg);
    asm volatile("dmb sy" ::: "memory");
    return val;
}

result_t gmac_init(void) {
    // 1. Reset DMA
    gmac_write(GMAC_DMA_BUS_MODE, 0x00000001);
    int timeout = 1000000;
    while ((gmac_read(GMAC_DMA_BUS_MODE) & 0x01) && timeout--) {
        asm volatile("yield");
    }
    if (timeout <= 0) return ERR_TIMEOUT;

    // 2. Configure MAC for basic operation
    gmac_write(GMAC_MAC_CONF, (1 << 14) | (1 << 11) | (1 << 3) | (1 << 2)); // PS, DM, TE, RE
    
    // 3. Set MAC Address (example: DE:AD:BE:EF:00:01)
    gmac_write(GMAC_MAC_ADDR0_HIGH, 0x00000100 | (0x00 << 8) | 0xEF);
    gmac_write(GMAC_MAC_ADDR0_LOW, (0xBE << 24) | (0xAD << 16) | (0xDE << 8));

    return OK;
}

result_t gmac_send_raw(const void* data, usize len) {
    // This will be expanded with DMA descriptor support
    // For "Hello World", we just prepare the infrastructure
    return ERR_NOT_FOUND; 
}
