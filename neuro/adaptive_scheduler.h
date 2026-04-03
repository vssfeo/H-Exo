// H-Exo Omni-Core: Adaptive Scheduler with Jitter Feedback
// Liquid Computing: Self-aware node migration based on stability metrics

#ifndef HEXO_NEURO_ADAPTIVE_SCHEDULER_H
#define HEXO_NEURO_ADAPTIVE_SCHEDULER_H

#include "../core/types.h"
#include "neuro_sync.h"
#include "../core/heartbeat.h"

// EMA (Exponential Moving Average) parameters
#define EMA_ALPHA_NUMERATOR 20      // α = 0.20 (20/100)
#define EMA_ALPHA_DENOMINATOR 100

// Adaptive scheduler state
typedef struct {
    neuro_sync_t* neural_arbitrator;
    heartbeat_stats_t* heartbeat_stats;
    
    // Self-awareness metrics
    u32 current_jitter_percent;
    u32 stability_score;        // 0-100, higher = more stable
    u32 ema_jitter;             // EMA-smoothed jitter value
    bool migration_recommended;
    
    // Historical tracking
    u64 total_inferences;
    u64 high_jitter_events;     // Count of jitter > 5%
    u64 last_migration_hint_time;
    
    // EMA state
    bool ema_initialized;
} adaptive_scheduler_t;

// Initialize adaptive scheduler
void adaptive_scheduler_init(adaptive_scheduler_t* sched, 
                             neuro_sync_t* neuro,
                             heartbeat_stats_t* hb_stats);

// Update scheduler with current jitter measurement
// Returns true if migration is recommended
bool adaptive_scheduler_update(adaptive_scheduler_t* sched, u32 jitter_percent);

// Run inference with jitter feedback
// Automatically adjusts telemetry based on stability
result_t adaptive_inference(adaptive_scheduler_t* sched,
                            telemetry_t* input,
                            inference_result_t* output);

// Get migration recommendation
bool should_migrate_task(adaptive_scheduler_t* sched);

// Calculate stability score (0-100)
u32 calculate_stability_score(adaptive_scheduler_t* sched);

#endif // HEXO_NEURO_ADAPTIVE_SCHEDULER_H
