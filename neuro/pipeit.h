// H-Exo Phase 2: Pipe-it Architecture
// 3-Stage Pipeline: Input -> Hidden -> Output
// SGI-driven wakeup (NOT SEV/WFE), WFI idle, A72 NEON for hidden+output
//
// Core assignment:
//   Core 0: Producer (submit frame, signal SGI)
//   Core 4: Hidden layer (A72 NEON SMLAL + PRFM prefetch)
//   Core 5: Output layer (A72 NEON SMLAL)
//   Cores 1-3: Idle / available for parallel hidden split (future)

#ifndef HEXO_PIPEIT_H
#define HEXO_PIPEIT_H

#include "../core/types.h"
#include "neuro_sync.h"

// Pipeline stages
#define PIPE_STAGE_INPUT    0   // Core 0: Input normalization + dispatch
#define PIPE_STAGE_HIDDEN   1   // Core 4: Hidden layer (A72 NEON)
#define PIPE_STAGE_OUTPUT   2   // Core 5: Output layer (A72 NEON)
#define PIPE_STAGE_COUNT    3

// Frame buffer slots (4 slots to avoid buffer-full in sequential submit+wait)
#define PIPE_BUF_COUNT      4

// Pipe-it frame state
typedef struct {
    u32 frame_id;                       // Frame sequence number
    telemetry_t input;                  // Raw telemetry input
    fixed_t hidden[NEURO_HIDDEN_SIZE];  // Hidden layer output
    inference_result_t result;          // Final output
    volatile u32 stage_complete;        // Bitmap: which stages done
    volatile u32 processing_core;       // Which core owns this frame
} pipe_frame_t;

// Double-buffered frame pool (cache-line aligned)
typedef struct __attribute__((aligned(64))) {
    pipe_frame_t frames[PIPE_BUF_COUNT];
    volatile u32 read_idx;              // Consumer reads from here
    volatile u32 write_idx;            // Producer writes here
    volatile u32 frame_counter;        // Global frame sequence
    volatile u32 dispatch_idx;         // Next frame for hidden worker
} pipe_buffer_t;

// Pipe-it controller
typedef struct {
    pipe_buffer_t* buffer;             // Double buffer
    const neural_weights_t* weights;   // Model weights
    volatile u32 active;               // Pipeline running flag
    u32 total_frames;                  // Total processed
    u32 dropped_frames;               // Buffer overflow count
    // PMU profiling for pipeline
    u64 hidden_cycles_sum;
    u64 output_cycles_sum;
    u64 hidden_count;
    u64 output_count;
} pipeit_t;

// Initialization and control
result_t pipeit_init(pipeit_t* pipe, pipe_buffer_t* buf, const neural_weights_t* weights);
void pipeit_start(pipeit_t* pipe);
void pipeit_stop(pipeit_t* pipe);

// Frame submission (called by Core 0)
result_t pipeit_submit_frame(pipeit_t* pipe, const telemetry_t* input, u32* frame_id);

// Stage completion (called by worker cores via SGI IRQ)
void pipeit_signal_stage(pipeit_t* pipe, u32 frame_idx, u32 stage);

// SGI IRQ handlers — called from handle_irq_exception on target core
void pipeit_sgi_hidden(u64 arg);
void pipeit_sgi_output(u64 arg);
void pipeit_sgi_done(u64 arg);

// WFI idle loop for pipeline workers (core 4, 5)
void pipeit_worker_idle_hidden(void);
void pipeit_worker_idle_output(void);

// Cross-cluster bell variables in Normal Non-Cacheable region @ PA 0x10000000
// (installed by mmu_install_nc_region(), see mmu_nc.c). Reads/writes go
// straight to DRAM bypassing all caches; no dc cvac/dc ivac needed; dsb ish
// is sufficient for ordering on Inner-Shareable observers.
//
// Layout (each spaced 64 bytes apart for clarity, NC has no false-sharing):
//   +0x00  completion_seq  worker -> producer (frame retired)
//   +0x40  hidden_pending  producer -> worker hidden (new frame ready)
//   +0x80  output_pending  producer -> worker output (unused, FUSED design)
#define PIPEIT_NC_BASE                     0x10000000ULL
#define g_pipeit_completion_seq            (*(volatile u64 *)(PIPEIT_NC_BASE + 0x00))
#define g_pipeit_hidden_pending            (*(volatile u64 *)(PIPEIT_NC_BASE + 0x40))
#define g_pipeit_output_pending            (*(volatile u64 *)(PIPEIT_NC_BASE + 0x80))

// STLR/LDAR helpers for cross-cluster bell synchronization (ARMv8 release/acquire).
// On Normal NC memory @ PIPEIT_NC_BASE, these compile to single STLR/LDAR
// instructions. Release semantics ensure all prior stores (incl. dc cvac of
// cacheable frame data) are observable to any LDAR observer before the bell
// store is observed. This eliminates the need for dsb sy on the publish path
// (~1-3us cross-cluster -> ~150ns single-instr).
//
// Reference: ARMv8 ARM "Load-Acquire/Store-Release"; "When a barrier does not
// block" (Arm Community); Liu et al. PPoPP 2020 "No Barrier in the Road".
#define PIPEIT_BELL_LOAD_ACQ(addr) \
    __atomic_load_n((u64 *)(addr), __ATOMIC_ACQUIRE)
#define PIPEIT_BELL_STORE_REL(addr, val) \
    __atomic_store_n((u64 *)(addr), (val), __ATOMIC_RELEASE)
#define PIPEIT_NC_COMPLETION_PTR  ((volatile u64 *)(PIPEIT_NC_BASE + 0x00))
#define PIPEIT_NC_HIDDEN_PTR      ((volatile u64 *)(PIPEIT_NC_BASE + 0x40))

// Per-phase latency instrumentation ([LOADTEST], 2026-10-03).
//   +0xC0  ts_pickup   worker saw the bell        (core 4 writes)
//   +0x100 ts_compute  fused hidden+output done   (core 4 writes)
//   +0x140 ts_bell     completion published       (core 4 writes)
// Value format: bits 47..0 = cntpct_el0 (24 MHz system counter, common to all
// PEs, so cross-core deltas are valid); bits 63..48 = tag, the low 16 bits of
// g_pipeit_hidden_pending at publish time. The producer validates the tag on
// all three slots before using them, so a frame whose timestamps were
// overwritten by a later one is excluded instead of silently skewing averages.
// NC stores are fire-and-forget and need no cache maintenance, so the probe
// adds no cross-cluster traffic of its own.
#define PIPEIT_NC_TS_PICKUP   (*(volatile u64 *)(PIPEIT_NC_BASE + 0xC0))

// IPC microbenchmark slots (mode 0..3 = poll/sev-wfe/sgi-wfi/sgi-spin), reused
// across mechanisms and directions. All in the NC region: no cache ops needed.
#define IPC_NC_ACTIVE (*(volatile u64 *)(PIPEIT_NC_BASE + 0x180))  /* phase gate   */
#define IPC_NC_MODE   (*(volatile u64 *)(PIPEIT_NC_BASE + 0x1C0))  /* mechanism    */
#define IPC_NC_CMD    (*(volatile u64 *)(PIPEIT_NC_BASE + 0x200))  /* command seq  */
#define IPC_NC_ACK    (*(volatile u64 *)(PIPEIT_NC_BASE + 0x240))  /* ack seq      */
#define IPC_NC_ROLE   (*(volatile u64 *)(PIPEIT_NC_BASE + 0x280))  /* respond core | (1<<8)=core4-init */
#define IPC_NC_PHASE  (*(volatile u64 *)(PIPEIT_NC_BASE + 0x2C0))  /* core4-init done */
#define IPC_NC_READY  (*(volatile u64 *)(PIPEIT_NC_BASE + 0x300))  /* target core entered phase */

// SGI chain witness, incremented on core 0 by pipeit_sgi_done()
// (see pipeit.c); poll mode never sends the DONE SGI.
extern volatile u64 g_pipeit_done_irq;
#define PIPEIT_NC_TS_COMPUTE  (*(volatile u64 *)(PIPEIT_NC_BASE + 0x100))
#define PIPEIT_NC_TS_BELL     (*(volatile u64 *)(PIPEIT_NC_BASE + 0x140))

#endif // HEXO_PIPEIT_H
