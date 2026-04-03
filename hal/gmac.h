#ifndef HEXO_HAL_GMAC_H
#define HEXO_HAL_GMAC_H

#include "../core/types.h"

// RK3399 GMAC Base Address
#define GMAC_BASE 0xFE300000

// Register Offsets (Simplified)
#define GMAC_MAC_CONF       0x0000
#define GMAC_MAC_FRAME_FLT  0x0004
#define GMAC_MAC_ADDR0_HIGH 0x0040
#define GMAC_MAC_ADDR0_LOW  0x0044
#define GMAC_DMA_BUS_MODE   0x1000
#define GMAC_DMA_TX_POLL    0x1004
#define GMAC_DMA_RX_POLL    0x1008
#define GMAC_DMA_TX_DESC    0x1010
#define GMAC_DMA_RX_DESC    0x1014
#define GMAC_DMA_STATUS     0x1018
#define GMAC_DMA_INTR_ENA   0x101C

result_t gmac_init(void);
result_t gmac_send_raw(const void* data, usize len);

#endif // HEXO_HAL_GMAC_H
