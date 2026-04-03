// H-Exo Omni-Core: Adaptive Scheduler Implementation
// Liquid Computing: Jitter-aware task migration

#include "adaptive_scheduler.h"

// Jitter threshold for migration recommendation
#define JITTER_THRESHOLD_PERCENT 5
#define STABILITY_DECAY_FACTOR 95  // 95% retention per update

void adaptive_scheduler_init(adaptive_scheduler_t* sched, 
                             neuro_sync_t* neuro,
                             heartbeat_stats_t* hb_stats) {
    if (!sched) return;
    
    sched->neural_arbitrator = neuro;
    sched->heartbeat_stats = hb_stats;
    sched->current_jitter_percent = 0;
    sched->stability_score = 100;  // Start optimistic
    sched->ema_jitter = 0;
    sched->migration_recommended = false;
    sched->total_inferences = 0;
    sched->high_jitter_events = 0;
    sched->last_migration_hint_time = 0;
    sched->ema_initialized = false;
}

bool adaptive_scheduler_update(adaptive_scheduler_t* sched, u32 jitter_percent) {
    if (!sched) return false;
    
    sched->current_jitter_percent = jitter_percent;
    
    // EMA smoothing: S_n = α·J_n + (1-α)·S_(n-1)
    // This prevents false positives from single jitter spikes
    if (!sched->ema_initialized) {
        sched->ema_jitter = jitter_percent;
        sched->ema_initialized = true;
    } else {
        // S_n = α·J_n + (1-α)·S_(n-1)
        // Using fixed-point: S_n = (α·J_n + (100-α)·S_(n-1)) / 100
        u32 alpha = EMA_ALPHA_NUMERATOR;
        u32 one_minus_alpha = EMA_ALPHA_DENOMINATOR - alpha;
        
        sched->ema_jitter = (alpha * jitter_percent + one_minus_alpha * sched->ema_jitter) 
                           / EMA_ALPHA_DENOMINATOR;
    }
    
    // Use EMA-smoothed jitter for decision making (not raw jitter)
    // This ignores single spikes and reacts only to systemic degradation
    if (sched->ema_jitter > JITTER_THRESHOLD_PERCENT) {
        sched->high_jitter_events++;
        sched->migration_recommended = true;
    } else {
        sched->migration_recommended = false;
    }
    
    // Update stability score based on EMA jitter
    if (sched->ema_jitter > JITTER_THRESHOLD_PERCENT) {
        // Penalty proportional to smoothed jitter
        u32 penalty = (sched->ema_jitter - JITTER_THRESHOLD_PERCENT) * 5;
        if (penalty > sched->stability_score) {
            sched->stability_score = 0;
        } else {
            sched->stability_score -= penalty;
        }
    } else {
        // Slow recovery towards 100
        sched->stability_score = (sched->stability_score * STABILITY_DECAY_FACTOR) / 100;
        if (sched->stability_score < 100) {
            sched->stability_score++;
        }
    }
    
    return sched->migration_recommended;
}

result_t adaptive_inference(adaptive_scheduler_t* sched,
                            telemetry_t* input,
                            inference_result_t* output) {
    if (!sched || !sched->neural_arbitrator || !input || !output) {
        return ERR_INVALID_PARAM;
    }
    
    // Inject jitter feedback into telemetry
    // High jitter = high thermal state (simulating stress)
    if (sched->current_jitter_percent > JITTER_THRESHOLD_PERCENT) {
        input->thermal_state = 80 + (sched->current_jitter_percent * 2);
        if (input->thermal_state > 100) input->thermal_state = 100;
    }
    
    // Run neural inference
    result_t res = neuro_sync_inference(sched->neural_arbitrator, input, output);
    if (res != OK) return res;
    
    sched->total_inferences++;
    
    // Override migration hint if jitter is critical
    if (sched->current_jitter_percent > JITTER_THRESHOLD_PERCENT) {
        output->migration_hint = 1;  // Recommend migration
    }
    
    // Override power state if stability is low
    if (sched->stability_score < 50) {
        output->power_state = 3;  // Maximum power/performance mode
    }
    
    return OK;
}

bool should_migrate_task(adaptive_scheduler_t* sched) {
    if (!sched) return false;
    
    // Migration recommended if:
    // 1. Current jitter exceeds threshold
    // 2. Stability score is critically low
    // 3. High jitter event rate > 20%
    
    if (sched->current_jitter_percent > JITTER_THRESHOLD_PERCENT) {
        return true;
    }
    
    if (sched->stability_score < 30) {
        return true;
    }
    
    if (sched->total_inferences > 10) {
        u32 jitter_rate = (sched->high_jitter_events * 100) / sched->total_inferences;
        if (jitter_rate > 20) {
            return true;
        }
    }
    
    return false;
}

u32 calculate_stability_score(adaptive_scheduler_t* sched) {
    if (!sched) return 0;
    return sched->stability_score;
}
