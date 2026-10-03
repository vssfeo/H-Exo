// H-Exo Omni-Core: Neural Arbitrator (Neuro-Sync)
// TinyML Inference Engine for Predictive Resource Management

#ifndef HEXO_NEURO_SYNC_H
#define HEXO_NEURO_SYNC_H

#include "../core/types.h"

// Fixed-point arithmetic (Q16.16 format)
typedef i32 fixed_t;

#define FIXED_SHIFT 16
#define FIXED_ONE (1 << FIXED_SHIFT)

// Convert integer to fixed-point.
//
// The shift is done on the UNSIGNED value on purpose. A left shift of a
// negative signed value is undefined behaviour (C11 6.5.7p4), and essentially
// every call site here passes a negative literal - the weight tables are full
// of INT_TO_FIXED(-1). Unsigned shift is fully defined, and the conversion back
// to fixed_t is the same two's-complement wraparound that every narrowing cast
// in this codebase already relies on.
#define INT_TO_FIXED(x) ((fixed_t)((u32)(x) * (u32)FIXED_ONE))

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
// Phase 1.3: 64-byte cache line aligned to prevent CCI-500 false sharing
// Each neuron's weights on separate cache line for parallel access
typedef struct __attribute__((aligned(64))) {
    // Layer 1: Input -> Hidden (8 neurons × 6 inputs)
    // Aligned to 64B: each neuron starts at cache line boundary
    fixed_t w1[NEURO_INPUT_SIZE][NEURO_HIDDEN_SIZE];
    fixed_t b1[NEURO_HIDDEN_SIZE];
    u8 pad1[64 - ((NEURO_INPUT_SIZE * NEURO_HIDDEN_SIZE + NEURO_HIDDEN_SIZE) * 4) % 64]; // pad to 64B
    
    // Layer 2: Hidden -> Output (4 neurons × 8 inputs)
    fixed_t w2[NEURO_HIDDEN_SIZE][NEURO_OUTPUT_SIZE];
    fixed_t b2[NEURO_OUTPUT_SIZE];
    u8 pad2[64 - ((NEURO_HIDDEN_SIZE * NEURO_OUTPUT_SIZE + NEURO_OUTPUT_SIZE) * 4) % 64]; // pad to 64B
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
neural_weights_t* neuro_sync_get_writable_weights(void);  // Phase 5.4: gossip access
result_t neuro_sync_inference(neuro_sync_t* ns, const telemetry_t* input, inference_result_t* output);
result_t neuro_sync_inference_a72(neuro_sync_t* ns, const telemetry_t* input, inference_result_t* output);
void neuro_sync_print_stats(neuro_sync_t* ns);

// Activation functions (fixed-point)
fixed_t relu(fixed_t x);
fixed_t sigmoid(fixed_t x);

#endif // HEXO_NEURO_SYNC_H
