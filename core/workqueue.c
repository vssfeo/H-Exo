// H-Exo Omni-Core: SMP Work Queue Implementation

#include "workqueue.h"

wq_slot_t wq_slots[SMP_MAX_CORES];
static volatile u64 wq_submit_cycles[SMP_MAX_CORES];
static volatile u64 wq_complete_cycles[SMP_MAX_CORES];
static volatile u32 wq_dispatch_counts[SMP_MAX_CORES];
static volatile u32 wq_complete_counts[SMP_MAX_CORES];

static inline u64 wq_read_cycles(void) {
    u64 val;
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

void wq_init(void) {
    for (u32 i = 0; i < SMP_MAX_CORES; i++) {
        wq_slots[i].ready = 0;
        wq_slots[i].done  = 1;   // slot free at startup
        wq_slots[i].fn    = (wq_fn_t)0;
        wq_slots[i].arg   = 0;
        wq_submit_cycles[i] = 0;
        wq_complete_cycles[i] = 0;
        wq_dispatch_counts[i] = 0;
        wq_complete_counts[i] = 0;
    }
    asm volatile("dmb ish" ::: "memory");
}

// Spin until the target core finishes its previous work, then arm the slot.
void wq_dispatch(u32 dst_core, wq_fn_t fn, u64 arg) {
    if (dst_core == 0 || dst_core >= SMP_MAX_CORES) return;
    wq_slot_t* s = &wq_slots[dst_core];
    // Wait for slot to be free (previous job done)
    while (!s->done) asm volatile("yield");
    s->done  = 0;
    s->fn    = fn;
    s->arg   = arg;
    wq_submit_cycles[dst_core] = wq_read_cycles();
    wq_dispatch_counts[dst_core]++;
    asm volatile("dmb ish" ::: "memory"); // ensure fn/arg visible before ready
    s->ready = 1;
    asm volatile("sev");                  // wake the secondary core if in WFE
}

// Non-blocking dispatch: arms slot only if it is free; returns 1 if dispatched.
u32 wq_try_dispatch(u32 dst_core, wq_fn_t fn, u64 arg) {
    if (dst_core == 0 || dst_core >= SMP_MAX_CORES) return 0;
    wq_slot_t* s = &wq_slots[dst_core];
    if (!s->done) return 0;          // previous job still running — skip
    s->done  = 0;
    s->fn    = fn;
    s->arg   = arg;
    wq_submit_cycles[dst_core] = wq_read_cycles();
    wq_dispatch_counts[dst_core]++;
    asm volatile("dmb ish" ::: "memory");
    s->ready = 1;
    asm volatile("sev");
    return 1;
}

// Called by secondary core in its spin loop.
void wq_worker_poll(u32 core_idx) {
    wq_slot_t* s = &wq_slots[core_idx];
    if (s->ready) {
        asm volatile("dmb ish" ::: "memory"); // ensure we see fn/arg
        wq_fn_t fn = s->fn;
        u64     arg = s->arg;
        s->ready = 0;
        if (fn) fn(arg);
        wq_complete_cycles[core_idx] = wq_read_cycles();
        wq_complete_counts[core_idx]++;
        asm volatile("dmb ish" ::: "memory"); // ensure callee writes visible
        s->done = 1;
    }
}

u32 wq_get_last_job_latency_us(u32 core_idx) {
    if (core_idx == 0 || core_idx >= SMP_MAX_CORES) {
        return 0;
    }
    if (wq_complete_counts[core_idx] == 0) {
        return 0;
    }
    if (wq_complete_cycles[core_idx] < wq_submit_cycles[core_idx]) {
        return 0;
    }
    return (u32)((wq_complete_cycles[core_idx] - wq_submit_cycles[core_idx]) / 24u);
}

u32 wq_get_dispatch_count(u32 core_idx) {
    if (core_idx >= SMP_MAX_CORES) {
        return 0;
    }
    return wq_dispatch_counts[core_idx];
}

u32 wq_get_complete_count(u32 core_idx) {
    if (core_idx >= SMP_MAX_CORES) {
        return 0;
    }
    return wq_complete_counts[core_idx];
}
