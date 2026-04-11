// H-Exo Omni-Core: EL2 MMU Identity Map
// 4 × 1GB L1 blocks: DRAM=Normal WB, MMIO=Device-nGnRnE

#ifndef MMU_H
#define MMU_H

void mmu_init(void);                   // Build L1 table, switch TTBR0_EL2, flush TLB
void mmu_enable(void);                 // Ensure SCTLR_EL2 M+C+I set (idempotent)
void mmu_disable(void);                // Clear M bit -- debug/emergency only
void mmu_secondary_el1_enable(void);   // EL1 MMU+D-cache for secondary cores

#endif // MMU_H
