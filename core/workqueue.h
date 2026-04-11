// H-Exo Omni-Core: SMP Work Queue
// Slot-based SPMC queue: core 0 dispatches, cores 1-3 execute.
// Each secondary core owns one slot — zero contention, no CAS needed.
// Data visible across cores via Normal WB IS DRAM + dmb ish.

#ifndef CORE_WORKQUEUE_H
#define CORE_WORKQUEUE_H

#include "types.h"
#include "smp.h"

typedef void (*wq_fn_t)(u64 arg);

// One cache-line-aligned slot per secondary core.
typedef struct {
    volatile u32  ready;   // 1 = work pending (written by core 0)
    volatile u32  done;    // 1 = slot free     (written by worker)
    wq_fn_t       fn;
    u64           arg;
} __attribute__((aligned(64))) wq_slot_t;

// Shared array indexed by core_idx (1..SMP_MAX_CORES-1).
extern wq_slot_t wq_slots[SMP_MAX_CORES];

// Init — zero all slots, mark all as done (free).
void wq_init(void);

// Dispatch fn(arg) to core dst_core (1..SMP_MAX_CORES-1).
// Blocks until the slot is free, then arms it.
void wq_dispatch(u32 dst_core, wq_fn_t fn, u64 arg);

// Non-blocking dispatch: skips if the slot is still busy.
// Returns 1 if dispatched, 0 if skipped. Safe to call from hot path.
u32  wq_try_dispatch(u32 dst_core, wq_fn_t fn, u64 arg);

// Called by each secondary core in its main loop.
// Polls its own slot; executes work and marks done.
void wq_worker_poll(u32 core_idx);

// Observability helpers for runtime policy / telemetry.
u32 wq_get_last_job_latency_us(u32 core_idx);
u32 wq_get_dispatch_count(u32 core_idx);
u32 wq_get_complete_count(u32 core_idx);

#endif // CORE_WORKQUEUE_H
