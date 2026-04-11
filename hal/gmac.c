// H-Exo Omni-Core: GMAC driver - Synopsys DWMAC on RK3399
// DMA descriptor-based TX, single-buffer single-frame

#include "gmac.h"
#include "uart.h"
#include "cache.h"

extern uart_t console;

// DMA TX descriptor (DWMAC normal mode: 4 x u32 = 16 bytes)
static volatile u32 tx_desc[4] __attribute__((aligned(16)));
static volatile u8  tx_buf[1520] __attribute__((aligned(32)));

// DMA RX descriptor ring (4 entries)
#define RX_DESC_COUNT 4
static volatile u32 rx_desc[RX_DESC_COUNT][4] __attribute__((aligned(16)));
static volatile u8  rx_buf[RX_DESC_COUNT][1520] __attribute__((aligned(32)));
static u32 rx_desc_idx;

static u8 mac_addr[6];

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
    // 1. Read MAC address programmed by U-Boot (don't overwrite)
    u32 hi = gmac_read(GMAC_MAC_ADDR0_HIGH);
    u32 lo = gmac_read(GMAC_MAC_ADDR0_LOW);
    mac_addr[0] = (u8)(lo >>  0);
    mac_addr[1] = (u8)(lo >>  8);
    mac_addr[2] = (u8)(lo >> 16);
    mac_addr[3] = (u8)(lo >> 24);
    mac_addr[4] = (u8)(hi >>  0);
    mac_addr[5] = (u8)(hi >>  8);

    // 2. Stop TX DMA cleanly (preserve U-Boot's DMA_BUS_MODE - no software reset).
    //    Bit 7 of DMA_BUS_MODE is ATDS (enhanced descriptors) on DWMAC 3.70a, NOT Fixed Burst.
    //    A software reset + wrong DMA_BUS_MODE write was inadvertently enabling enhanced
    //    (8-word) descriptors while we supply normal 4-word descriptors, stalling the DMA.
    u32 op_mode = gmac_read(GMAC_DMA_OP_MODE);
    op_mode &= ~(1u << 13);  // ST=0: stop TX DMA before pointing it at our ring
    op_mode &= ~(1u << 1);   // SR=0: keep RX DMA off
    gmac_write(GMAC_DMA_OP_MODE, op_mode);

    // 3. Set up TX descriptor ring (CPU owns, end-of-ring, buffer pre-assigned)
    tx_desc[0] = 0;                         // OWN=0 (CPU owns)
    tx_desc[1] = (1u << 25);                // TER (end of ring)
    tx_desc[2] = (u32)(uintptr_t)tx_buf;
    tx_desc[3] = 0;
    dcache_flush((void*)tx_desc, 16);       // flush to RAM so DMA sees our descriptor

    // 4. Point TX DMA at our descriptor ring
    gmac_write(GMAC_DMA_TX_DESC, (u32)(uintptr_t)tx_desc);

    // 5. Enable TX in MAC_CONF - read-modify-write to preserve U-Boot's
    //    speed (PS/FES) and duplex (DM) and RGMII settings
    u32 mac_conf = gmac_read(GMAC_MAC_CONF);
    mac_conf |= (1u << 3);   // TE = Transmitter Enable
    gmac_write(GMAC_MAC_CONF, mac_conf);

    // 6. Start TX DMA with Store-and-Forward (TSF, bit21).
    //    TSF ensures full frame is in FIFO before TX starts - needed for small frames
    //    (our 32-byte beacon < default 64-byte threshold in non-TSF mode).
    op_mode = gmac_read(GMAC_DMA_OP_MODE);
    op_mode &= ~(1u << 1);   // SR=0: no RX DMA
    op_mode |=  (1u << 13);  // ST=1:  Start TX DMA
    op_mode |=  (1u << 21);  // TSF=1: TX Store-and-Forward
    gmac_write(GMAC_DMA_OP_MODE, op_mode);

    // 7. Set up RX descriptor ring: 4 entries, each owns a 1520-byte buffer
    rx_desc_idx = 0;
    for (u32 i = 0; i < RX_DESC_COUNT; i++) {
        rx_desc[i][0] = (1u << 31);                  // OWN=1: DMA owns
        rx_desc[i][1] = (1520u & 0x7FFu);            // RBS1=1520 bytes
        if (i == RX_DESC_COUNT - 1)
            rx_desc[i][1] |= (1u << 15);             // RER: end of ring (last descriptor)
        rx_desc[i][2] = (u32)(uintptr_t)rx_buf[i];   // buffer address
        rx_desc[i][3] = 0;
        dcache_flush((void*)rx_desc[i], 16);         // flush each RX desc to RAM
        dcache_flush((void*)rx_buf[i], 1520);        // flush RX buffers (DMA will write here)
    }

    // 8. Start RX DMA - point to descriptor ring, enable SR and RSF
    gmac_write(GMAC_DMA_RX_DESC, (u32)(uintptr_t)rx_desc);
    op_mode = gmac_read(GMAC_DMA_OP_MODE);
    op_mode |=  (1u << 1);   // SR=1:  Start RX DMA
    op_mode |=  (1u << 25);  // RSF=1: RX Store-and-Forward (full frame before forwarding)
    gmac_write(GMAC_DMA_OP_MODE, op_mode);

    return OK;
}

result_t gmac_send_raw(const void* data, usize len) {
    if (!data || len == 0 || len > 1514) return ERR_INVALID_PARAM;

    // Descriptor must be owned by CPU before we can write
    if (tx_desc[0] & (1u << 31)) return ERR_HARDWARE_FAULT;

    // Copy payload to DMA buffer
    const u8* src = (const u8*)data;
    for (usize i = 0; i < len; i++) {
        tx_buf[i] = src[i];
    }
    // Flush tx_buf to RAM so DMA can read the frame payload
    dcache_flush((void*)tx_buf, len);

    // Configure descriptor: LS=1(bit30), FS=1(bit29), TER=1(bit25), CIC=0(no cksum), TBS1=len
    tx_desc[1] = (1u << 30) | (1u << 29) | (1u << 25) | (len & 0x7FFu);
    tx_desc[2] = (u32)(uintptr_t)tx_buf;
    tx_desc[3] = 0;

    // Hand descriptor to DMA (OWN bit must be last write) then flush to RAM
    tx_desc[0] = (1u << 31);
    dcache_flush((void*)tx_desc, 16);   // critical: DMA must see OWN=1 in physical RAM

    // Demand poll to wake TX DMA
    gmac_write(GMAC_DMA_TX_POLL, 1);

    // Wait for TX complete: DMA clears OWN in physical RAM - invalidate cache each poll
    int timeout = 200000;
    while (timeout--) {
        dcache_invalidate((void*)tx_desc, 16);
        if (!(tx_desc[0] & (1u << 31))) break;
        asm volatile("yield");
    }
    if (timeout <= 0) {
        u32 st = gmac_read(GMAC_DMA_STATUS);
        uart_puts(&console, "[DBG] GMAC TX timeout: STATUS=0x"); uart_put_hex(&console, st);
        uart_puts(&console, " TS="); uart_put_hex(&console, (st >> 20) & 7);
        uart_puts(&console, " OP_MODE=0x"); uart_put_hex(&console, gmac_read(GMAC_DMA_OP_MODE));
        uart_puts(&console, " MAC_CONF=0x"); uart_put_hex(&console, gmac_read(GMAC_MAC_CONF));
        uart_puts(&console, "\r\n");
        uart_puts(&console, "[DBG] tx_desc[0]=0x"); uart_put_hex(&console, tx_desc[0]);
        uart_puts(&console, " [1]=0x"); uart_put_hex(&console, tx_desc[1]);
        uart_puts(&console, " [2]=0x"); uart_put_hex(&console, tx_desc[2]);
        uart_puts(&console, "\r\n");
        return ERR_TIMEOUT;
    }

    return OK;
}

// Non-blocking receive: returns ERR_NOT_FOUND if no frame ready.
// On success: copies frame into buf, sets *len (without FCS), advances ring.
result_t gmac_recv_raw(u8* buf, usize* len) {
    if (!buf || !len) return ERR_INVALID_PARAM;

    volatile u32* desc = rx_desc[rx_desc_idx];

    // Invalidate before reading: DMA may have updated OWN in physical RAM
    dcache_invalidate((void*)desc, 16);

    // DMA still owns this descriptor - no frame ready
    if (desc[0] & (1u << 31)) return ERR_NOT_FOUND;

    // Error summary bit: discard and recycle descriptor
    if (desc[0] & (1u << 15)) {
        desc[0] = (1u << 31);
        dcache_flush((void*)desc, 16);
        rx_desc_idx = (rx_desc_idx + 1) % RX_DESC_COUNT;
        return ERR_HARDWARE_FAULT;
    }

    // Frame length is in RDES0 bits[29:16]; includes 4-byte FCS
    u32 fl = (desc[0] >> 16) & 0x3FFFu;
    usize copy_len = (fl > 4) ? (fl - 4) : 0;
    if (copy_len > 1520) copy_len = 1520;
    *len = copy_len;

    // Invalidate rx_buf so CPU sees what DMA wrote to physical RAM
    dcache_invalidate((void*)rx_buf[rx_desc_idx], copy_len);
    const u8* src = (const u8*)rx_buf[rx_desc_idx];
    for (usize i = 0; i < copy_len; i++) buf[i] = src[i];

    // Recycle descriptor back to DMA: set OWN=1 and flush to RAM
    desc[0] = (1u << 31);
    dcache_flush((void*)desc, 16);
    rx_desc_idx = (rx_desc_idx + 1) % RX_DESC_COUNT;

    // Demand poll to resume RX DMA if it suspended waiting for descriptors
    gmac_write(GMAC_DMA_RX_POLL, 1);

    return OK;
}

const u8* gmac_get_mac(void) {
    return mac_addr;
}

// Enable GMAC DMA RX interrupt in the DWMAC IP.
// Must call gicv3_enable_irq(GMAC_GIC_INTID) separately.
void gmac_irq_enable(void) {
    gmac_write(GMAC_DMA_INTR_ENA, GMAC_INTR_RIE | GMAC_INTR_NIE);
}

// Clear DMA_STATUS interrupt bits so GIC de-asserts the SPI line.
void gmac_clear_irq(void) {
    u32 st = gmac_read(GMAC_DMA_STATUS);
    gmac_write(GMAC_DMA_STATUS, st & (GMAC_DMA_STATUS_RI | GMAC_DMA_STATUS_NIS));
}
