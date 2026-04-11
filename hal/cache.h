// H-Exo Omni-Core: D-cache coherency primitives
// Used by DMA drivers (GMAC) and MMU setup for CPU<->DMA coherency.
// U-Boot leaves D-cache enabled at EL2; all DMA-accessed memory must be
// explicitly flushed (CPU→RAM) or invalidated (RAM→CPU) around DMA ops.
//
// ARM cache line size: 64 bytes (A72/A53)
// DC CVAC: Clean by VA to PoC  – flush dirty cache to RAM (before DMA read)
// DC IVAC: Invalidate by VA to PoC – drop cache line (before CPU reads DMA write)

#ifndef HAL_CACHE_H
#define HAL_CACHE_H

#include "../core/types.h"

#define CACHE_LINE_SIZE 64UL

// Flush [addr, addr+len) to physical RAM so DMA can read CPU writes.
static inline void dcache_flush(const void* addr, usize len) {
    uintptr_t a   = (uintptr_t)addr & ~(CACHE_LINE_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;
    for (; a < end; a += CACHE_LINE_SIZE)
        asm volatile("dc cvac, %0" :: "r"(a) : "memory");
    asm volatile("dsb ish" ::: "memory");
}

// Invalidate [addr, addr+len) so CPU reads DMA-written data from RAM.
static inline void dcache_invalidate(const void* addr, usize len) {
    uintptr_t a   = (uintptr_t)addr & ~(CACHE_LINE_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;
    for (; a < end; a += CACHE_LINE_SIZE)
        asm volatile("dc ivac, %0" :: "r"(a) : "memory");
    asm volatile("dsb ish" ::: "memory");
}

// Clean-and-invalidate [addr, addr+len) — use when recycling DMA buffers.
static inline void dcache_flush_invalidate(const void* addr, usize len) {
    uintptr_t a   = (uintptr_t)addr & ~(CACHE_LINE_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + len;
    for (; a < end; a += CACHE_LINE_SIZE)
        asm volatile("dc civac, %0" :: "r"(a) : "memory");
    asm volatile("dsb ish" ::: "memory");
}

#endif // HAL_CACHE_H
