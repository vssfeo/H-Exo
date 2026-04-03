// H-Exo Omni-Core: Neural Arbitrator (Neuro-Sync)
// TinyML Inference Engine for Predictive Resource Management

#ifndef HEXO_NEURO_SYNC_H
#define HEXO_NEURO_SYNC_H

#include "../core/types.h"

// Fixed-point arithmetic (Q16.16 format)
typedef i32 fixed_t;

#define FIXED_SHIFT 16
#define FIXED_ONE (1 << FIXED_SHIFT)

// Convert integer to fixed-point
#define INT_TO_FIXED(x) ((x) << FIXED_SHIFT)

// Convert fixed-point to integer
#define FIXED_TO_INT(x) ((x) >> FIXED_SHIFT)

// Fixed-point multiplication
static inline fixed_t fixed_mul(fixed_t a, fixed_t b) {
    return (fixed_t)(((i64)a * (i64)b) >> FIXED_SHIFT);
}

// Telemetry input structure
typedef struct {
    u32 cpu_load;           // CPU load percentage (0-100)
    u32 l2_latency_us;      // L2 link latency in microseconds
    u32 memory_pressure;    // Memory usage percentage (0-100)
    u32 thermal_state;      // Temperature reading (0-100 scale)
    u32 packet_rate;        // Packets per second on L2 mesh
    u32 node_count;         // Number of active nodes in cluster
} telemetry_t;

// Neural network output
typedef struct {
    u8  task_priority;      // Predicted task priority (0-255)
    u8  migration_hint;     // 0=stay, 1=migrate to high-perf, 2=migrate to low-power
    u8  power_state;        // Predicted power state (0=sleep, 1=idle, 2=active, 3=turbo)
    u8  trust_score;        // Node reliability score (0-255)
} inference_result_t;

// Neural network configuration
#define NEURO_INPUT_SIZE    6
#define NEURO_HIDDEN_SIZE   8
#define NEURO_OUTPUT_SIZE   4

// Neural network weights (pre-trained, embedded in ROM)
typedef struct {
    // Layer 1: Input -> Hidden
    fixed_t w1[NEURO_INPUT_SIZE][NEURO_HIDDEN_SIZE];
    fixed_t b1[NEURO_HIDDEN_SIZE];
    
    // Layer 2: Hidden -> Output
    fixed_t w2[NEURO_HIDDEN_SIZE][NEURO_OUTPUT_SIZE];
    fixed_t b2[NEURO_OUTPUT_SIZE];
} neural_weights_t;

// Neural Arbitrator state
typedef struct {
    const neural_weights_t* weights;
    telemetry_t last_telemetry;
    inference_result_t last_result;
    u32 inference_count;
    bool initialized;
} neuro_sync_t;

// API
result_t neuro_sync_init(neuro_sync_t* ns);
result_t neuro_sync_inference(neuro_sync_t* ns, const telemetry_t* input, inference_result_t* output);
void neuro_sync_print_stats(neuro_sync_t* ns);

// Activation functions (fixed-point)
fixed_t relu(fixed_t x);
fixed_t sigmoid(fixed_t x);

#endif // HEXO_NEURO_SYNC_H
