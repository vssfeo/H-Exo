// H-Exo Omni-Core: Artificial Chaos Generator
// Stress testing for adaptive scheduler and jitter measurement

#ifndef HEXO_CORE_CHAOS_H
#define HEXO_CORE_CHAOS_H

#include "types.h"

// Chaos modes
typedef enum {
    CHAOS_NONE = 0,
    CHAOS_CACHE_DISABLE,    // Disable L1 caches temporarily
    CHAOS_NOP_STORM,        // Execute heavy NOP cycles
    CHAOS_MEMORY_THRASH,    // Random memory access patterns
    CHAOS_INTERRUPT_FLOOD   // Trigger software interrupts
} chaos_mode_t;

// Chaos configuration
typedef struct {
    chaos_mode_t mode;
    u32 intensity;          // 0-100, higher = more chaos
    u32 duration_ms;        // How long to apply chaos
    bool active;
} chaos_config_t;

// Initialize chaos generator
void chaos_init(void);

// Apply chaos for stress testing
void chaos_apply(chaos_mode_t mode, u32 intensity, u32 duration_ms);

// Stop all chaos
void chaos_stop(void);

// Cache manipulation functions
void chaos_disable_caches(void);
void chaos_enable_caches(void);

// NOP storm (burns CPU cycles)
void chaos_nop_storm(u32 iterations);

// Memory thrashing (cache pollution)
void chaos_memory_thrash(u32 size_kb);

#endif // HEXO_CORE_CHAOS_H
