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
#define GICR_ISPENDR0   (GICR_SGI_OFFSET + 0x0200)
#define GICR_ICPENDR0   (GICR_SGI_OFFSET + 0x0280)
#define GICR_IPRIORITYR0 (GICR_SGI_OFFSET + 0x0400)
#define GICR_IGRPMODR0  (GICR_SGI_OFFSET + 0x0D00)

// GMAC DMA interrupt: GIC SPI 24 → INTID 56
#define GMAC_GIC_INTID  56u

void     gicv3_prewake_redistributors(void);
u32      gicv3_read_waker(u32 core);
u32      gicv3_force_wake_core(u32 core, u32 retries);
result_t gicv3_init(void);
void     gicv3_init_cpu_iface(void);  // Phase 2: per-core CPU interface init
void gicv3_enable_irq(u32 irq);
void gicv3_route_irq(u32 irq, u64 affinity);  // set GICD_IROUTER
void gicv3_set_priority(u32 irq, u8 prio);    // set GICD_IPRIORITYR
u32  gicv3_ack_irq(void);                       // read ICC_IAR1_EL1
void gicv3_eoi_irq(u32 intid);                 // write ICC_EOIR1_EL1

// Phase 2: GICv3 SGI (Software Generated Interrupts) for inter-core wakeup
// Targeted IPI: sends SGI to specific cores
void gicv3_sgi_init(void);                     // Initialize SGI (configure as non-secure group 1)
void gicv3_sgi_send(u32 sgi_id, u64 target_aff); // Send SGI to specific affinity
void gicv3_sgi_send_to_list(u32 sgi_id, u16 core_list); // Send to bitmask of cores (bits 0-5)
u32  gicv3_sgi_ack(void);                      // Acknowledge SGI, return source core

// Diagnostic helpers — read raw GIC state from any core
u32  gicv3_read_ispendr0(u32 core);            // GICR_ISPENDR0 of given core (SGI/PPI pending)
u32  gicv3_read_gicd_ctlr(void);               // GICD_CTLR
u64  gicv3_read_gicd_typer(void);              // GICD_TYPER

extern volatile u64 g_gicv3_sgi_send_count;
extern volatile u64 g_gicv3_sgi_last_val;
extern volatile u64 g_gicv3_sgi_last_id;
extern volatile u64 g_gicv3_sgi_last_aff;

// SGI IDs for Pipe-it pipeline stages
#define SGI_STAGE_INPUT     0   // Core 0 -> Core 1/2: Input stage complete
#define SGI_STAGE_HIDDEN    1   // Core 1/2 -> Core 3/4: Hidden stage complete
#define SGI_STAGE_OUTPUT    2   // Core 3/4 -> Core 5: Output stage complete
#define SGI_STAGE_DONE      3   // Core 5 -> Core 1: Frame complete

#endif // HEXO_HAL_GICV3_H
