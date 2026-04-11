#ifndef HEXO_HAL_GMAC_H
#define HEXO_HAL_GMAC_H

#include "../core/types.h"

// RK3399 GMAC Base Address
#define GMAC_BASE 0xFE300000

// H-Exo EtherType (L2 mesh frames)
#define HEXO_ETHERTYPE      0x88EE

// MAC Configuration Registers
#define GMAC_MAC_CONF       0x0000
#define GMAC_MAC_FRAME_FLT  0x0004
#define GMAC_MAC_ADDR0_HIGH 0x0040
#define GMAC_MAC_ADDR0_LOW  0x0044

// DMA Registers (Synopsys DWMAC offsets from GMAC_BASE)
#define GMAC_DMA_BUS_MODE   0x1000
#define GMAC_DMA_TX_POLL    0x1004
#define GMAC_DMA_RX_POLL    0x1008
#define GMAC_DMA_RX_DESC    0x100C
#define GMAC_DMA_TX_DESC    0x1010
#define GMAC_DMA_STATUS     0x1014
#define GMAC_DMA_OP_MODE    0x1018
#define GMAC_DMA_INTR_ENA   0x101C

// DMA_STATUS bits
#define GMAC_DMA_STATUS_RI  (1u << 6)   // Receive Interrupt
#define GMAC_DMA_STATUS_NIS (1u << 16)  // Normal Interrupt Summary

// DMA_INTR_ENA bits
#define GMAC_INTR_RIE (1u << 6)   // Receive Interrupt Enable
#define GMAC_INTR_NIE (1u << 16)  // Normal Interrupt Summary Enable

result_t  gmac_init(void);
void      gmac_irq_enable(void);               // enable DMA RX interrupt in DWMAC
result_t  gmac_send_raw(const void* data, usize len);
result_t  gmac_recv_raw(u8* buf, usize* len);   // non-blocking; ERR_NOT_FOUND if no frame
const u8* gmac_get_mac(void);
void      gmac_clear_irq(void);                // clear DMA_STATUS interrupt bits

#endif // HEXO_HAL_GMAC_H
