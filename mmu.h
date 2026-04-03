// H-Exo Omni-Core: Memory Dominance Module
// MMU Interface for distributed address space foundation

#ifndef MMU_H
#define MMU_H

#include <stdint.h>

// Initialize MMU page tables
// Maps:
//   0x00000000-0x7FFFFFFF -> Normal Cacheable (RAM)
//   0xC0000000-0xFFFFFFFF -> Device Memory (Peripherals)
void mmu_init(void);

// Enable MMU and caches
// Activates the distributed address space
void mmu_enable(void);

// Disable MMU (for debugging)
void mmu_disable(void);

#endif // MMU_H
