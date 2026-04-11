// H-Exo Omni-Core: SMP wakeup via PSCI
// Brings up secondary cores on RK3399 using ARM PSCI SMC calls

#ifndef HEXO_CORE_SMP_H
#define HEXO_CORE_SMP_H

#include "types.h"
#include "../hal/uart.h"

#define SMP_MAX_CORES 6

typedef enum {
    SMP_PATH_NOT_REQUESTED = 0,
    SMP_PATH_PRE_KERNEL_HANDOFF,
    SMP_PATH_REACHED_START,
    SMP_PATH_REACHED_SECONDARY_ENTRY,
    SMP_PATH_REACHED_C_WORKER,
} smp_path_state_t;

// Per-core idle counters (incremented by secondary cores, read by core 0)
extern volatile u64 smp_idle_counters[SMP_MAX_CORES];
// Per-core ASM entry stages written from secondary_entry in boot.s.
extern volatile u64 smp_entry_stage[SMP_MAX_CORES];
// MPIDR of first non-primary core that reached _start (written in boot.s).
extern volatile u64 smp_start_tombstone;

// Bring up all secondary cores via PSCI CPU_ON
result_t smp_init(void);

// Returns number of cores successfully started (including core 0)
u32 smp_get_online_count(void);
// Returns first online secondary core index (1..SMP_MAX_CORES-1), or 0 if none.
u32 smp_get_first_secondary_online(void);
// Bitmask of secondary cores that reached smp_secondary_main() C loop.
u32 smp_get_secondary_enter_mask(void);
// Returns first secondary core that reached smp_secondary_main(), or 0 if none.
u32 smp_get_first_secondary_entered(void);
// Returns total number of active compute nodes: core0 + secondaries that reached C worker.
u32 smp_get_active_node_count(void);
// Classify how far a secondary core progressed in the bring-up chain.
smp_path_state_t smp_classify_core(u32 core_idx);

// Secondary core C entry point (called from boot.s secondary_entry)
void smp_secondary_main(u64 core_idx);

// Dump consolidated PSCI + ASM trace telemetry to UART.
void smp_dump_diagnostics(uart_t *uart);

#endif // HEXO_CORE_SMP_H
