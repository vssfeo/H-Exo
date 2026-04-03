#ifndef HEXO_HAL_GICV3_H
#define HEXO_HAL_GICV3_H

#include "../core/types.h"

// RK3399 GICv3 Base Addresses
#define GICD_BASE       0xFEE00000  // Distributor
#define GICR_BASE       0xFEF00000  // Redistributor

// GIC Distributor Register Offsets
#define GICD_CTLR       0x0000
#define GICD_TYPER      0x0004
#define GICD_IIDR       0x0008
#define GICD_IGROUPR    0x0080
#define GICD_ISENABLER  0x0100
#define GICD_ICENABLER  0x0180
#define GICD_ISPENDR    0x0200
#define GICD_ICPENDR    0x0280
#define GICD_ISACTIVER  0x0300
#define GICD_ICACTIVER  0x0380
#define GICD_IPRIORITYR 0x0400
#define GICD_ITARGETSR  0x0800
#define GICD_ICFGR      0x0C00
#define GICD_IROUTER    0x6000

// GIC Redistributor Register Offsets
#define GICR_CTLR       0x0000
#define GICR_IIDR       0x0004
#define GICR_TYPER      0x0008
#define GICR_WAKER      0x0014

// GICR SGI Base (Redistributor SGI and PPI)
#define GICR_SGI_OFFSET 0x10000
#define GICR_IGROUPR0   (GICR_SGI_OFFSET + 0x0080)
#define GICR_ISENABLER0 (GICR_SGI_OFFSET + 0x0100)
#define GICR_ICENABLER0 (GICR_SGI_OFFSET + 0x0180)
#define GICR_IPRIORITYR0 (GICR_SGI_OFFSET + 0x0400)

result_t gicv3_init(void);
void gicv3_enable_irq(u32 irq);

#endif // HEXO_HAL_GICV3_H
