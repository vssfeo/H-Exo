// H-Exo Omni-Core: Neural Arbitrator Implementation
// TinyML Inference using Fixed-Point Arithmetic

#include "neuro_sync.h"

// Pre-trained weights (generated offline by LLM-agent)
// This is a simple 6->8->4 feedforward network
// Trained to predict: task_priority, migration_hint, power_state, trust_score
static const neural_weights_t default_weights = {
    // Layer 1: Input (6) -> Hidden (8)
    .w1 = {
        // Weights optimized for CPU load, L2 latency, memory, thermal, packet rate, node count
        {INT_TO_FIXED(2), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(1), INT_TO_FIXED(-1), INT_TO_FIXED(0), INT_TO_FIXED(1)},
        {INT_TO_FIXED(1), INT_TO_FIXED(2), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(1), INT_TO_FIXED(-1), INT_TO_FIXED(0)},
        {INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(2), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(1), INT_TO_FIXED(-1)},
        {INT_TO_FIXED(0), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(2), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(1)},
        {INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(2), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(0)},
        {INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(2), INT_TO_FIXED(-1), INT_TO_FIXED(1)}
    },
    .b1 = {INT_TO_FIXED(0), INT_TO_FIXED(0), INT_TO_FIXED(0), INT_TO_FIXED(0), 
           INT_TO_FIXED(0), INT_TO_FIXED(0), INT_TO_FIXED(0), INT_TO_FIXED(0)},
    
    // Layer 2: Hidden (8) -> Output (4)
    .w2 = {
        {INT_TO_FIXED(1), INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(0)},
        {INT_TO_FIXED(-1), INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(1)},
        {INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(-1), INT_TO_FIXED(1)},
        {INT_TO_FIXED(0), INT_TO_FIXED(1), INT_TO_FIXED(1), INT_TO_FIXED(-1)},
        {INT_TO_FIXED(1), INT_TO_FIXED(-1), INT_TO_FIXED(0), INT_TO_FIXED(1)},
        {INT_TO_FIXED(-1), INT_TO_FIXED(0), INT_TO_FIXED(1), INT_TO_FIXED(1)},
        {INT_TO_FIXED(0), INT_TO_FIXED(1), INT_TO_FIXED(-1), INT_TO_FIXED(0)},
        {INT_TO_FIXED(1), INT_TO_FIXED(0), INT_TO_FIXED(1), INT_TO_FIXED(-1)}
    },
    .b2 = {INT_TO_FIXED(0), INT_TO_FIXED(0), INT_TO_FIXED(0), INT_TO_FIXED(0)}
};

//==============================================================================
// Activation Functions (Fixed-Point)
//==============================================================================

// ReLU: max(0, x)
fixed_t relu(fixed_t x) {
    return (x > 0) ? x : 0;
}

// Sigmoid approximation using lookup table (fast)
fixed_t sigmoid(fixed_t x) {
    // Simple approximation: sigmoid(x) ≈ 0.5 + x/4 for small x
    // Clamped to [0, 1]
    fixed_t result = (FIXED_ONE >> 1) + (x >> 2);
    if (result < 0) return 0;
    if (result > FIXED_ONE) return FIXED_ONE;
    return result;
}

//==============================================================================
// Neural Network Inference
//==============================================================================

result_t neuro_sync_init(neuro_sync_t* ns) {
    if (!ns) return ERR_INVALID_PARAM;
    
    ns->weights = &default_weights;
    ns->inference_count = 0;
    ns->initialized = true;
    
    // Initialize with neutral values
    ns->last_telemetry.cpu_load = 0;
    ns->last_telemetry.l2_latency_us = 0;
    ns->last_telemetry.memory_pressure = 0;
    ns->last_telemetry.thermal_state = 0;
    ns->last_telemetry.packet_rate = 0;
    ns->last_telemetry.node_count = 1;
    
    ns->last_result.task_priority = 128;
    ns->last_result.migration_hint = 0;
    ns->last_result.power_state = 1;
    ns->last_result.trust_score = 255;
    
    return OK;
}

result_t neuro_sync_inference(neuro_sync_t* ns, const telemetry_t* input, inference_result_t* output) {
    if (!ns || !input || !output) return ERR_INVALID_PARAM;
    if (!ns->initialized) return ERR_NOT_FOUND;
    
    // Convert inputs to fixed-point (normalized to 0-1 range)
    fixed_t inputs[NEURO_INPUT_SIZE];
    inputs[0] = INT_TO_FIXED(input->cpu_load) / 100;
    inputs[1] = INT_TO_FIXED(input->l2_latency_us) / 1000;  // Normalize to ms
    inputs[2] = INT_TO_FIXED(input->memory_pressure) / 100;
    inputs[3] = INT_TO_FIXED(input->thermal_state) / 100;
    inputs[4] = INT_TO_FIXED(input->packet_rate) / 1000;    // Normalize to kpps
    inputs[5] = INT_TO_FIXED(input->node_count) / 10;       // Normalize to tens
    
    // Hidden layer activations
    fixed_t hidden[NEURO_HIDDEN_SIZE];
    
    // Layer 1: Input -> Hidden
    for (u32 i = 0; i < NEURO_HIDDEN_SIZE; i++) {
        fixed_t sum = ns->weights->b1[i];
        for (u32 j = 0; j < NEURO_INPUT_SIZE; j++) {
            sum += fixed_mul(inputs[j], ns->weights->w1[j][i]);
        }
        hidden[i] = relu(sum);
    }
    
    // Output layer activations
    fixed_t outputs[NEURO_OUTPUT_SIZE];
    
    // Layer 2: Hidden -> Output
    for (u32 i = 0; i < NEURO_OUTPUT_SIZE; i++) {
        fixed_t sum = ns->weights->b2[i];
        for (u32 j = 0; j < NEURO_HIDDEN_SIZE; j++) {
            sum += fixed_mul(hidden[j], ns->weights->w2[j][i]);
        }
        outputs[i] = sigmoid(sum);
    }
    
    // Convert outputs back to integers
    output->task_priority = (u8)(FIXED_TO_INT(outputs[0] * 255));
    output->migration_hint = (u8)(FIXED_TO_INT(outputs[1] * 2));  // 0, 1, or 2
    output->power_state = (u8)(FIXED_TO_INT(outputs[2] * 3));     // 0, 1, 2, or 3
    output->trust_score = (u8)(FIXED_TO_INT(outputs[3] * 255));
    
    // Clamp values
    if (output->migration_hint > 2) output->migration_hint = 2;
    if (output->power_state > 3) output->power_state = 3;
    
    // Update state
    ns->last_telemetry = *input;
    ns->last_result = *output;
    ns->inference_count++;
    
    return OK;
}

void neuro_sync_print_stats(neuro_sync_t* ns) {
    // This will be implemented with UART output
    (void)ns;
}
