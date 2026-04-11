// H-Exo Omni-Core: Telemetry Collection Implementation

#include "telemetry.h"
#include "../core/slab.h"
#include "../core/smp.h"
#include "../core/workqueue.h"

#define TELEMETRY_WINDOW_CYCLES 2400000ULL  // 100ms @ 24MHz

static u32 telemetry_clamp_percent(u32 value) {
    return (value > 100u) ? 100u : value;
}

result_t telemetry_init(telemetry_collector_t* tc) {
    if (!tc) return ERR_INVALID_PARAM;
    
    // cntpct_el0 (generic timer) requires no setup - always accessible at EL1
    tc->last_cycle_count = read_cycle_counter();
    tc->last_sample_cycles = tc->last_cycle_count;
    tc->total_cycles = 0;
    tc->packets_total = 0;
    tc->last_packet_snapshot = 0;
    tc->sample_count = 0;
    
    tc->current.cpu_load = 0;
    tc->current.l2_latency_us = 0;
    tc->current.memory_pressure = 0;
    tc->current.thermal_state = 35;  // cool idle baseline
    tc->current.packet_rate = 0;
    tc->current.node_count = 1;
    
    return OK;
}

void telemetry_note_packet(telemetry_collector_t* tc) {
    if (!tc) {
        return;
    }
    tc->packets_total++;
}

u32 telemetry_get_cpu_load(telemetry_collector_t* tc) {
    u64 current_cycles = read_cycle_counter();
    u64 delta = current_cycles - tc->last_cycle_count;
    tc->last_cycle_count = current_cycles;
    tc->total_cycles += delta;
    
    // Generic timer at 24MHz; 100ms = 2,400,000 ticks
    // Load estimate: proportion of time spent active vs expected 100ms window
    u32 load = (u32)((delta * 100) / 2400000);
    if (load > 100) load = 100;
    
    return load;
}

u32 telemetry_get_l2_latency(void) {
    u32 worker_core = smp_get_first_secondary_entered();
    if (!worker_core) {
        return 0;
    }
    return wq_get_last_job_latency_us(worker_core);
}

u32 telemetry_get_memory_pressure(void) {
    return slab_get_usage_percent();
}

u32 telemetry_get_packet_rate(telemetry_collector_t* tc) {
    if (!tc) return 0;

    u64 now = read_cycle_counter();
    u64 delta_cycles = now - tc->last_sample_cycles;
    u64 delta_packets = tc->packets_total - tc->last_packet_snapshot;

    if (delta_cycles == 0) {
        return tc->current.packet_rate;
    }

    return (u32)((delta_packets * 24000000ULL) / delta_cycles);
}

u32 telemetry_get_node_count(void) {
    return smp_get_active_node_count();
}

u32 telemetry_get_thermal_state(telemetry_collector_t* tc) {
    if (!tc) return 0;

    // Activity-based estimate until TSADC is wired in:
    // more CPU load, packet traffic, and active nodes imply higher thermal stress.
    u32 thermal = 30u;
    thermal += tc->current.cpu_load / 2u;
    thermal += (tc->current.packet_rate > 1000u) ? 30u : (tc->current.packet_rate / 40u);
    if (tc->current.node_count > 1u) {
        thermal += (tc->current.node_count - 1u) * 5u;
    }
    return telemetry_clamp_percent(thermal);
}

result_t telemetry_collect(telemetry_collector_t* tc, telemetry_t* output) {
    if (!tc || !output) return ERR_INVALID_PARAM;
    
    output->cpu_load = telemetry_get_cpu_load(tc);
    output->l2_latency_us = telemetry_get_l2_latency();
    output->memory_pressure = telemetry_get_memory_pressure();
    output->packet_rate = telemetry_get_packet_rate(tc);
    output->node_count = telemetry_get_node_count();
    tc->current = *output;
    output->thermal_state = telemetry_get_thermal_state(tc);
    tc->current = *output;
    tc->last_sample_cycles = read_cycle_counter();
    tc->last_packet_snapshot = tc->packets_total;
    
    tc->sample_count++;
    
    return OK;
}
