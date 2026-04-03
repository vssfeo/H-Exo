// H-Exo Omni-Core: Telemetry Collection System
// Real-time metrics for Neural Arbitrator

#ifndef HEXO_TELEMETRY_H
#define HEXO_TELEMETRY_H

#include "../core/types.h"
#include "neuro_sync.h"

// Telemetry collector state
typedef struct {
    u64 last_cycle_count;
    u64 total_cycles;
    u32 sample_count;
    telemetry_t current;
} telemetry_collector_t;

// Initialize telemetry system
result_t telemetry_init(telemetry_collector_t* tc);

// Collect current system telemetry
result_t telemetry_collect(telemetry_collector_t* tc, telemetry_t* output);

// Read CPU cycle counter (PMU)
static inline u64 read_cycle_counter(void) {
    u64 val;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(val));
    return val;
}

// Read CPU load (simplified - based on cycle counter delta)
u32 telemetry_get_cpu_load(telemetry_collector_t* tc);

// Estimate L2 latency (placeholder - will use actual L2 mesh when available)
u32 telemetry_get_l2_latency(void);

// Get memory pressure (simplified - will use actual allocator when available)
u32 telemetry_get_memory_pressure(void);

// Get thermal state (placeholder - will read actual thermal sensor)
u32 telemetry_get_thermal_state(void);

#endif // HEXO_TELEMETRY_H
