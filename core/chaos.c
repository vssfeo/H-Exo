// H-Exo Omni-Core: Artificial Chaos Generator Implementation

#include "chaos.h"

static chaos_config_t chaos_state;

void chaos_init(void) {
    chaos_state.mode = CHAOS_NONE;
    chaos_state.intensity = 0;
    chaos_state.duration_ms = 0;
    chaos_state.active = false;
}

void chaos_apply(chaos_mode_t mode, u32 intensity, u32 duration_ms) {
    chaos_state.mode = mode;
    chaos_state.intensity = intensity;
    chaos_state.duration_ms = duration_ms;
    chaos_state.active = true;
    
    switch (mode) {
        case CHAOS_CACHE_DISABLE:
            chaos_disable_caches();
            break;
            
        case CHAOS_NOP_STORM:
            // Scale iterations by intensity (0-100)
            chaos_nop_storm(intensity * 10000);
            break;
            
        case CHAOS_MEMORY_THRASH:
            // Scale memory size by intensity
            chaos_memory_thrash(intensity);
            break;
            
        default:
            break;
    }
}

void chaos_stop(void) {
    if (chaos_state.mode == CHAOS_CACHE_DISABLE) {
        chaos_enable_caches();
    }
    
    chaos_state.active = false;
    chaos_state.mode = CHAOS_NONE;
}

void chaos_disable_caches(void) {
    // Disable L1 D-Cache
    u64 sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr &= ~(1 << 2);  // Clear C bit (D-Cache)
    asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    asm volatile("isb");
}

void chaos_enable_caches(void) {
    // Re-enable L1 D-Cache
    u64 sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1 << 2);  // Set C bit (D-Cache)
    asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    asm volatile("isb");
}

void chaos_nop_storm(u32 iterations) {
    // Burn CPU cycles with NOPs
    for (u32 i = 0; i < iterations; i++) {
        asm volatile("nop");
        asm volatile("nop");
        asm volatile("nop");
        asm volatile("nop");
        asm volatile("nop");
        asm volatile("nop");
        asm volatile("nop");
        asm volatile("nop");
    }
}

void chaos_memory_thrash(u32 size_kb) {
    // Allocate on stack and thrash cache with random access
    volatile u8 buffer[1024];
    
    for (u32 kb = 0; kb < size_kb; kb++) {
        for (u32 i = 0; i < 1024; i++) {
            // Random-ish access pattern to pollute cache
            u32 idx = (i * 17 + kb * 37) % 1024;
            buffer[idx] = (u8)(i + kb);
        }
    }
    (void)buffer[0];
}
