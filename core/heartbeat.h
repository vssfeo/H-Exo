// H-Exo Omni-Core: Heartbeat Stability Monitor
// Cycle-accurate timing for jitter measurement and stability testing

#ifndef HEXO_CORE_HEARTBEAT_H
#define HEXO_CORE_HEARTBEAT_H

#include "types.h"
#include "../hal/uart.h"

// Heartbeat configuration
#define HEARTBEAT_INTERVAL_MS 100  // 100ms between beats
#define HEARTBEAT_CYCLES_24MHZ 2400000ULL  // 100ms @ 24MHz Generic Timer

// Heartbeat statistics
typedef struct {
    u64 beat_count;
    u64 total_cycles;
    u64 min_interval_cycles;
    u64 max_interval_cycles;
    u64 last_beat_cycles;
    u32 jitter_percent;  // Max deviation from expected interval
} heartbeat_stats_t;

// Initialize heartbeat system
void heartbeat_init(uart_t* uart);

// Run heartbeat mode (infinite loop)
void heartbeat_run(uart_t* uart);

// Get current statistics
void heartbeat_get_stats(heartbeat_stats_t* stats);

// Print heartbeat statistics
void heartbeat_print_stats(uart_t* uart);

// Enable/disable chaos mode for stress testing
void heartbeat_enable_chaos(bool enable);

#endif // HEXO_CORE_HEARTBEAT_H
