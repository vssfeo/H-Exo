// H-Exo Phase 2: Pipe-it Implementation
// 3-Stage Pipeline with GICv3 SGI signaling
// Core 0: Producer | Core 4: Hidden (A72 NEON) | Core 5: Output (A72 NEON)
// Workers idle via WFI, woken by targeted SGI (NOT SEV broadcast)

#include "pipeit.h"
#include "a72_opt.h"
#include "../hal/gicv3.h"
#include "../hal/pmu.h"
#include "../hal/uart.h"
#include "../core/workqueue.h"

extern uart_t console;

// Global pipe-it instance for SGI handlers
static pipeit_t* g_pipe = NULL;
static pipe_buffer_t* g_buffer = NULL;
static u32 g_pipeit_use_sgi_a72 = 0;

#define RK_SIP_GICR_WAKER_GET_64 0xC20000A2ULL
#define RK_SIP_GICR_WAKE_TRY_64  0xC20000A3ULL

static u64 pipeit_sip_gicr_get(u64 core, u64* waker)
{
    register u64 x0 asm("x0") = RK_SIP_GICR_WAKER_GET_64;
    register u64 x1 asm("x1") = core;
    register u64 x2 asm("x2") = 0;
    register u64 x3 asm("x3") = 0;
    asm volatile("smc #0"
                 : "+r"(x0), "+r"(x1)
                 : "r"(x2), "r"(x3)
                 : "memory",
                   "x4", "x5", "x6", "x7", "x8", "x9",
                   "x10", "x11", "x12", "x13", "x14", "x15",
                   "x16", "x17");
    if (waker) *waker = x1;
    return x0;
}

/* Kept unused on purpose: this is the EL3 wake-toggle path, measured to time
 * out on the stuck A72 frames (flags=0x2). Retained so the attempt and its SMC
 * encoding stay documented; called by nobody since 2026-10-03. */
static u64 __attribute__((unused)) pipeit_sip_gicr_wake_try(u64 core, u64* before, u64* after)
{
    register u64 x0 asm("x0") = RK_SIP_GICR_WAKE_TRY_64;
    register u64 x1 asm("x1") = core;
    register u64 x2 asm("x2") = 0;
    register u64 x3 asm("x3") = 0;
    asm volatile("smc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2)
                 : "r"(x3)
                 : "memory",
                   "x4", "x5", "x6", "x7", "x8", "x9",
                   "x10", "x11", "x12", "x13", "x14", "x15",
                   "x16", "x17");
    if (before) *before = x1;
    if (after) *after = x2;
    return x0;
}

// Phase 2.1 (Plan v3.2 §2.1): Dual-path IPC primitive.
//
// Primary wake: SEV (system event). RK3399 SEV is broadcast across the CCI
// bridge and uses the per-PE event register, NOT the GIC. This works even
// when A72 GICR.ChildrenAsleep is latched (silicon errata, see ADL-014/015).
//
// Secondary wake: targeted SGI to the A72 cluster, ONLY when the boot-time
// probe (pipeit_select_a72_sgi_mode) confirmed the secure-world GICR can be
// woken cleanly. Otherwise SGI is suppressed to avoid spurious IAR on the
// idle worker (which would burn cycles re-acknowledging an interrupt that
// the CPU never actually pended).
//
// Caller contract: data stores + dc cvac + dsb sy MUST have completed
// before invoking this helper, otherwise the worker may observe the bell
// before the data lands in DRAM (CCI A53<->A72 is not snooping for NS
// writes on RK3399 — empirical, B2.2.4 cache maint ordering).
static inline void pipeit_ipi_signal_a72(u32 sgi_id, u32 target_aff)
{
    asm volatile("sev" ::: "memory");
    if (g_pipeit_use_sgi_a72) {
        gicv3_sgi_send(sgi_id, target_aff);
    }
}

static u32 pipeit_select_a72_sgi_mode(void)
{
    u64 get4_st = 0, get5_st = 0, get4_w = 0, get5_w = 0;
    u64 wake4_fl = 0, wake5_fl = 0, wake4_before = 0, wake5_before = 0;
    u64 wake4_after = 0, wake5_after = 0;

    /* wake_try REMOVED (2026-10-03): it drives PS 1->0->1 on frames 4/5 from
     * EL3. Both directions are measured-ignored on the stuck A72 frames, and a
     * successful toggle after the power-cycle test would RE-STICK them. The
     * read-only GET below still reports the frame state. 0xEEEE marks
     * "not attempted" in the log line. */
    wake4_fl = 0xEEEEu;
    wake5_fl = 0xEEEEu;
    get4_st = pipeit_sip_gicr_get(4, &get4_w);
    get5_st = pipeit_sip_gicr_get(5, &get5_w);

    uart_puts(&console, "[PIPEIT][GICR_SIP] wake4 fl=0x");
    uart_put_hex(&console, wake4_fl);
    uart_puts(&console, " b=0x");
    uart_put_hex(&console, wake4_before);
    uart_puts(&console, " a=0x");
    uart_put_hex(&console, wake4_after);
    uart_puts(&console, " | wake5 fl=0x");
    uart_put_hex(&console, wake5_fl);
    uart_puts(&console, " b=0x");
    uart_put_hex(&console, wake5_before);
    uart_puts(&console, " a=0x");
    uart_put_hex(&console, wake5_after);
    uart_puts(&console, "\r\n");

    uart_puts(&console, "[PIPEIT][GICR_SIP] get4 st=0x");
    uart_put_hex(&console, get4_st);
    uart_puts(&console, " w=0x");
    uart_put_hex(&console, get4_w);
    uart_puts(&console, " | get5 st=0x");
    uart_put_hex(&console, get5_st);
    uart_puts(&console, " w=0x");
    uart_put_hex(&console, get5_w);
    uart_puts(&console, "\r\n");

    // SiP patch absent or unsupported -> use legacy NS-side WAKER heuristic.
    if (get4_st == 0xFFFFFFFFULL || get5_st == 0xFFFFFFFFULL) {
        u32 w4 = gicv3_read_waker(4);
        u32 w5 = gicv3_read_waker(5);
        u32 ca_stuck = ((w4 | w5) & (1u << 2)) ? 1u : 0u;
        return (ca_stuck == 0u) ? 1u : 0u;
    }

    // Architectural criterion: the secure GET snapshot shows ChildrenAsleep
    // clear on both A72 frames, i.e. the redistributor<->PE link is up and an
    // SGI can be presented. The old variant additionally required a successful
    // secure wake_try; that call is gone - it wrote to the very frames whose
    // state this function certifies, and a certification must not mutate what
    // it certifies. (Root cause of the whole A72 interrupt saga, 2026-10-03:
    // H-Exo's own prewake wrote ProcessorSleep=0 to the A72 frames while the
    // cores were OFF, wedging the GIC wake transition so BL31's later
    // mark_core_awake no-opped and ChildrenAsleep stayed 1 forever. With the
    // prewake off those frames, BL31 completes the handshake by itself.)
    if (get4_st == 0 && get5_st == 0 &&
        (get4_w & (1ULL << 2)) == 0 && (get5_w & (1ULL << 2)) == 0) {
        /* Delivery IS certified. But enabling the SGI-driven pipeline on the
         * strength of that alone was measured premature (2026-10-03, first live
         * run with mode=1): 317/1000 frames completed in 34 s against 1000/1000
         * in ~11 ms for polling, with sgi_sent output=0 done=0 - the stage chain
         * hidden->output->done is not wired end to end and had never executed
         * before, because CA=1 made this selector return 0 on every prior boot.
         * Polling stays the shipped mode until the chain completes a bench with
         * zero timeouts. The capability is reported so the log shows why. */
        uart_puts(&console, "[PIPEIT] SGI delivery certified; chain unproven "
                            "(output/done sends=0 on first live run) - polling mode\r\n");
        return 0u;
    }

    return 0u;
}

// Exported for smp_secondary_main to check if pipeline is active
volatile u32 g_pipeit_active = 0;

// Diagnostic counters: SGI send count per stage (producer side, core 0)
volatile u64 g_pipeit_sgi_sent[4] = {0, 0, 0, 0};
// Wake mode diagnostic: 0 = mailbox(SEV/WFE) only, 1 = mailbox + SGI assist.
volatile u64 g_pipeit_sgi_a72_enabled = 0;
// Memory-polling pipeline counters: kept as the fallback path. (A72 SGI
// delivery was believed broken on RK3399; it was our own prewake wedging
// GICR_WAKER - fixed 2026-10-03. Polling remains the safe mode.)
// (GICR.ChildrenAsleep stuck on cluster 1, ICC_HPPIR1 returns spurious),
// so we use these monotonic counters as the wake/dispatch signal instead.
//
// These bells now live in a Normal Non-Cacheable 2MB region @ PA 0x10000000
// (see mmu_nc.c, mmu_install_nc_region()). Symbols are macros declared in
// pipeit.h; reads/writes go straight to DRAM, no dc cvac/dc ivac needed.
// Diagnostic counters: handler entry/exit per stage (worker side)
volatile u64 g_pipeit_hidden_handler_enter = 0;
volatile u64 g_pipeit_hidden_handler_exit  = 0;
volatile u64 g_pipeit_output_handler_enter = 0;
volatile u64 g_pipeit_output_handler_exit  = 0;

// Get frame index from write/read idx (wraps around PIPE_BUF_COUNT)
#define FRAME_IDX(idx) ((idx) % PIPE_BUF_COUNT)

// Initialize pipe-it pipeline
result_t pipeit_init(pipeit_t* pipe, pipe_buffer_t* buf, const neural_weights_t* weights) {
    if (!pipe || !buf || !weights) return ERR_INVALID_PARAM;
    
    pipe->buffer = buf;
    pipe->weights = weights;
    pipe->active = 0;
    pipe->total_frames = 0;
    pipe->dropped_frames = 0;
    pipe->hidden_cycles_sum = 0;
    pipe->output_cycles_sum = 0;
    pipe->hidden_count = 0;
    pipe->output_count = 0;
    
    // Clear buffer
    for (u32 i = 0; i < PIPE_BUF_COUNT; i++) {
        buf->frames[i].frame_id = 0;
        buf->frames[i].stage_complete = 0;
        buf->frames[i].processing_core = 0xFF;
    }
    buf->read_idx = 0;
    buf->write_idx = 0;
    buf->frame_counter = 0;
    buf->dispatch_idx = 0;
    
    // Initialize GICv3 SGI routing
    gicv3_sgi_init();
    
    g_pipe = pipe;
    g_buffer = buf;
    
    return OK;
}

// Start pipeline — workers are already in WFI loop on their cores
void pipeit_start(pipeit_t* pipe) {
    if (!pipe) return;
    pipe->active = 1;
    g_pipeit_active = 1;
    g_pipeit_sgi_sent[SGI_STAGE_INPUT] = 0;
    g_pipeit_sgi_sent[SGI_STAGE_HIDDEN] = 0;
    g_pipeit_sgi_sent[SGI_STAGE_OUTPUT] = 0;
    g_pipeit_sgi_sent[SGI_STAGE_DONE] = 0;
    // Deterministic start state for NC bells (producer/worker mailboxes).
    // If previous run left stale values, workers can consume a phantom event
    // and bench logic can wait on old completion sequence.
    g_pipeit_completion_seq = 0;
    g_pipeit_hidden_pending = 0;
    g_pipeit_output_pending = 0;
    // Runtime SGI mode selection for A72 workers.
    // Prefer secure-world wake/get probes. If BL31 patch is absent, fallback to
    // legacy non-secure WAKER heuristic.
    {
        g_pipeit_use_sgi_a72 = pipeit_select_a72_sgi_mode();
        g_pipeit_sgi_a72_enabled = g_pipeit_use_sgi_a72;
        uart_puts(&console, "[PIPEIT] A72 SGI mode=0x");
        uart_put_hex(&console, g_pipeit_use_sgi_a72);
        uart_puts(&console, "\r\n");
    }
    // Inter-cluster coherency workaround: A72 cluster does NOT snoop A53
    // cluster's writes (SMPEN cannot be set from EL2 on RK3399). Force the
    // value to DRAM with cache-clean, then SEV. Cores 4/5 will invalidate
    // their copy before reading on each loop iteration (see smp.c).
    asm volatile("dc cvac, %0" :: "r"(&g_pipeit_active) : "memory");
    asm volatile("dc cvac, %0" :: "r"(&pipe->active)   : "memory");
    asm volatile("dsb sy" ::: "memory");
    // Phase 2.1: kick both A72 workers via dual-path IPC. Hidden first so
    // the producer side has visibility in PIPE_DIAG; output is opportunistic
    // (worker stays in WFE until first dispatch).
    pipeit_ipi_signal_a72(SGI_STAGE_HIDDEN, 0x100);
    pipeit_ipi_signal_a72(SGI_STAGE_OUTPUT, 0x101);
    uart_puts(&console, "[PIPEIT] Pipeline started\r\n");
}

// Stop pipeline — wake workers via SEV so they observe active=0 and exit.
void pipeit_stop(pipeit_t* pipe) {
    if (!pipe) return;
    pipe->active = 0;
    g_pipeit_active = 0;
    // Push the active=0 write through to DRAM so A72 cluster sees it after dc-ivac.
    asm volatile("dc cvac, %0" :: "r"(&pipe->active)         : "memory");
    asm volatile("dc cvac, %0" :: "r"(&g_pipeit_active)      : "memory");
    asm volatile("dsb sy" ::: "memory");
    // Phase 2.1: dual-path wake so workers exit WFE and observe active=0.
    pipeit_ipi_signal_a72(SGI_STAGE_HIDDEN, 0x100);
    pipeit_ipi_signal_a72(SGI_STAGE_OUTPUT, 0x101);
    uart_puts(&console, "[PIPEIT] Pipeline stopped\r\n");
}

// Submit new frame (Core 0 — producer)
result_t pipeit_submit_frame(pipeit_t* pipe, const telemetry_t* input, u32* frame_id) {
    if (!pipe || !input) return ERR_INVALID_PARAM;
    // Accept either active indicator to avoid transient desync between
    // cacheable global flag and per-instance active state during start.
    if (!g_pipeit_active && !pipe->active) return ERR_INVALID_PARAM;
    
    pipe_buffer_t* buf = pipe->buffer;
    u32 write_idx = buf->write_idx;
    u32 read_idx = buf->read_idx;
    
    // Check for buffer full (write catches read with 1 slot gap)
    if (((write_idx + 1) % PIPE_BUF_COUNT) == (read_idx % PIPE_BUF_COUNT)) {
        pipe->dropped_frames++;
        return ERR_OUT_OF_MEMORY;
    }
    
    // Get frame slot
    u32 frame_idx = FRAME_IDX(write_idx);
    pipe_frame_t* frame = &buf->frames[frame_idx];
    
    // Initialize frame — clear all stage bits first (stale OUTPUT from prev frame)
    buf->frame_counter++;
    frame->frame_id = buf->frame_counter;
    frame->input = *input;
    frame->stage_complete = 0;
    asm volatile("dmb ish" ::: "memory");
    frame->stage_complete = 1u << PIPE_STAGE_INPUT;  // Input ready
    frame->processing_core = 0;
    
    if (frame_id) *frame_id = frame->frame_id;
    
    // Advance write pointer
    buf->write_idx = (write_idx + 1) % PIPE_BUF_COUNT;
    
    // Update dispatch index for hidden worker
    buf->dispatch_idx = frame_idx;
    asm volatile("dmb ish" ::: "memory");
    
    // PRIMARY signal: lock-free WFE/SEV mailbox.
    // RK3399 CCI-500 does NOT snoop A53↔A72, so cache maintenance is mandatory:
    // we must dc cvac the data + seq so they hit DRAM, and consumer dc ivac to
    // re-fetch. Confirmed empirically: removing cvac drops completion to ~1%.
    asm volatile("dc cvac, %0" :: "r"(&buf->frames[frame_idx])  : "memory");
    asm volatile("dc cvac, %0" :: "r"(&buf->dispatch_idx)       : "memory");
    asm volatile("dmb ish" ::: "memory");
    // dsb sy required: ARMv8 ARM B2.2.4 says cache maintenance (dc cvac) is
    // NOT synchronized by STLR; only dsb (sy or ish) waits for cvac to drain
    // to PoC. Without it, worker may observe new bell via LDAR before frame
    // data lands in DRAM => stale read on A72 side. Empirical: STLR-only path
    // costs +1.2us/frame from retry/stall (12.17us vs 10.99us with dsb sy).
    g_pipeit_hidden_pending++;
    asm volatile("dsb sy" ::: "memory");
    // Phase 2.1: dual-path IPC. SEV mailbox is the primary wake; SGI is a
    // secondary assist when GICR probe says it is safe (mode_sgi_a72=1).
    // Counter is incremented only when SGI was actually sent so PIPE_DIAG
    // sgi_sent[] reflects on-wire IPC traffic, not policy intent.
    pipeit_ipi_signal_a72(SGI_STAGE_HIDDEN, 0x100);
    if (g_pipeit_use_sgi_a72) {
        g_pipeit_sgi_sent[SGI_STAGE_HIDDEN]++;
    }
    return OK;
}

// Signal stage completion — called from SGI IRQ handler on worker core
void pipeit_signal_stage(pipeit_t* pipe, u32 frame_idx, u32 stage) {
    if (!pipe || frame_idx >= PIPE_BUF_COUNT) return;
    
    pipe_frame_t* frame = &pipe->buffer->frames[frame_idx];
    frame->stage_complete |= (1u << stage);
    asm volatile("dmb ish" ::: "memory");
    
    switch (stage) {
        case PIPE_STAGE_HIDDEN:
            // FUSED design (Idea #2): output runs inline on core 4 in
            // pipeit_sgi_hidden. We never signal HIDDEN stage to core 5 — it
            // would just be a wasted cross-CCU IPC. Core 5 worker stays in WFE.
            // This branch is therefore a no-op kept for ABI compat.
            break;
            
        case PIPE_STAGE_OUTPUT:
            pipe->total_frames++;
            pipe->buffer->read_idx = 
                (pipe->buffer->read_idx + 1) % PIPE_BUF_COUNT;
            // Bell increment: NC memory (see pipeit.h). No dc cvac needed.
            // total_frames and read_idx are still cacheable for diagnostic
            // continuity; we cvac them but A53 producer waits on the bell.
            g_pipeit_completion_seq++;
            asm volatile("dc cvac, %0" :: "r"(&pipe->total_frames)        : "memory");
            asm volatile("dc cvac, %0" :: "r"(&pipe->buffer->read_idx)    : "memory");
            // dsb sy required: cacheable cvac of total_frames/read_idx must
            // drain to DRAM before producer's LDAR observes the bell. STLR
            // alone does NOT order with cvac (ARMv8 B2.2.4) -> regression.
            asm volatile("dsb sy" ::: "memory");
            // (Idea #4) SGI_STAGE_DONE to core 0 removed: producer polls
            // total_frames directly, the IRQ handler was empty (no-op). Keep
            // g_pipeit_sgi_sent[] producer-owned to avoid cross-cluster cache
            // line contention corrupting hidden SGI diagnostics.
            break;
    }
}

// FUSED handler (Idea #2): runs BOTH hidden and output layers on core 4.
// Eliminates one cross-CCU IPC round-trip (saved ~4us). The intermediate
// `frame->hidden` array stays in core 4's L1 across the two compute calls,
// so output layer's reads are L1 hits instead of L3/DRAM round-trips.
void pipeit_sgi_hidden(u64 arg) {
    (void)arg;
    g_pipeit_hidden_handler_enter++;
    if (!g_pipe || !g_pipeit_active) return;
    
    pipe_buffer_t* buf = g_pipe->buffer;
    u32 frame_idx = buf->dispatch_idx;
    if (frame_idx >= PIPE_BUF_COUNT) return;
    
    pipe_frame_t* frame = &buf->frames[frame_idx];
    
    // Skip if hidden already done for this frame
    if (frame->stage_complete & (1u << PIPE_STAGE_HIDDEN)) return;
    
    frame->processing_core = 4;
    
    const neural_weights_t* w = g_pipe->weights;
    
    // Convert telemetry to fixed_t array
    fixed_t inputs[NEURO_INPUT_SIZE];
    inputs[0] = (fixed_t)frame->input.cpu_load;
    inputs[1] = (fixed_t)frame->input.l2_latency_us;
    inputs[2] = (fixed_t)frame->input.memory_pressure;
    inputs[3] = (fixed_t)frame->input.thermal_state;
    inputs[4] = (fixed_t)frame->input.packet_rate;
    inputs[5] = (fixed_t)frame->input.node_count;
    
    asm volatile("prfm pldl1keep, [%0]" :: "r"(w->w1));
    asm volatile("prfm pldl1keep, [%0]" :: "r"(w->w2));
    
    // === Stage 1: hidden layer ===
    u64 cyc_start, cyc_mid, cyc_end;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(cyc_start));
    a72_hidden_layer_neon(
        inputs,
        (const fixed_t*)w->w1,
        w->b1,
        frame->hidden,
        NEURO_INPUT_SIZE,
        NEURO_HIDDEN_SIZE
    );
    asm volatile("mrs %0, pmccntr_el0" : "=r"(cyc_mid));
    g_pipe->hidden_cycles_sum += (cyc_mid - cyc_start);
    g_pipe->hidden_count++;
    frame->stage_complete |= (1u << PIPE_STAGE_HIDDEN);
    
    // === Stage 2: output layer (FUSED inline, hidden array hot in L1) ===
    fixed_t outputs[NEURO_OUTPUT_SIZE];
    a72_output_layer_unrolled(
        frame->hidden,
        (const fixed_t*)w->w2,
        w->b2,
        outputs
    );
    asm volatile("mrs %0, pmccntr_el0" : "=r"(cyc_end));
    g_pipe->output_cycles_sum += (cyc_end - cyc_mid);
    g_pipe->output_count++;
    
    for (u32 i = 0; i < NEURO_OUTPUT_SIZE; i++) {
        outputs[i] = sigmoid(outputs[i]);
    }
    frame->result.task_priority = (u8)(FIXED_TO_INT(outputs[0] * 255));
    frame->result.migration_hint = (u8)(FIXED_TO_INT(outputs[1] * 2));
    frame->result.power_state = (u8)(FIXED_TO_INT(outputs[2] * 3));
    frame->result.trust_score = (u8)(FIXED_TO_INT(outputs[3] * 255));
    if (frame->result.migration_hint > 2) frame->result.migration_hint = 2;
    if (frame->result.power_state > 3) frame->result.power_state = 3;
    
    // Retire frame: signal OUTPUT (advances read_idx + SGI to core 0).
    pipeit_signal_stage(g_pipe, frame_idx, PIPE_STAGE_OUTPUT);
    g_pipeit_hidden_handler_exit++;
    g_pipeit_output_handler_enter++;
    g_pipeit_output_handler_exit++;
}

// SGI handler: Output layer (runs on Core 5 via IRQ)
void pipeit_sgi_output(u64 arg) {
    (void)arg;
    g_pipeit_output_handler_enter++;
    if (!g_pipe || !g_pipeit_active) return;
    
    pipe_buffer_t* buf = g_pipe->buffer;
    // Output processes the same frame that hidden just completed
    u32 frame_idx = buf->dispatch_idx;
    if (frame_idx >= PIPE_BUF_COUNT) return;
    
    pipe_frame_t* frame = &buf->frames[frame_idx];
    
    // Skip if output already done
    if (frame->stage_complete & (1u << PIPE_STAGE_OUTPUT)) return;
    
    frame->processing_core = 5;
    
    const neural_weights_t* w = g_pipe->weights;
    fixed_t outputs[NEURO_OUTPUT_SIZE];
    
    // Per-stage PMU profiling: measure output layer cycles
    u64 cyc_start, cyc_end;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(cyc_start));
    
    // A72 NEON output layer (SMLAL, fully unrolled)
    a72_output_layer_unrolled(
        frame->hidden,
        (const fixed_t*)w->w2,
        w->b2,
        outputs
    );
    
    asm volatile("mrs %0, pmccntr_el0" : "=r"(cyc_end));
    g_pipe->output_cycles_sum += (cyc_end - cyc_start);
    g_pipe->output_count++;
    
    // Apply sigmoid and store results
    for (u32 i = 0; i < NEURO_OUTPUT_SIZE; i++) {
        outputs[i] = sigmoid(outputs[i]);
    }
    
    frame->result.task_priority = (u8)(FIXED_TO_INT(outputs[0] * 255));
    frame->result.migration_hint = (u8)(FIXED_TO_INT(outputs[1] * 2));
    frame->result.power_state = (u8)(FIXED_TO_INT(outputs[2] * 3));
    frame->result.trust_score = (u8)(FIXED_TO_INT(outputs[3] * 255));
    
    if (frame->result.migration_hint > 2) frame->result.migration_hint = 2;
    if (frame->result.power_state > 3) frame->result.power_state = 3;
    
    // Signal completion → frame retired
    pipeit_signal_stage(g_pipe, frame_idx, PIPE_STAGE_OUTPUT);
    g_pipeit_output_handler_exit++;
}

// SGI handler: Frame done (runs on Core 0)
void pipeit_sgi_done(u64 arg) {
    (void)arg;
    // Frame retired — nothing to do, backpressure already released
}

// Diagnostic counters for poll-vs-vector path. If polled_hits > 0 and the
// handler_*_enter counters stay 0, the GIC delivers SGIs but the architected
// IRQ vector is never invoked => problem is in vectors.s / VBAR / EL routing.
volatile u64 g_pipeit_poll_hits_hidden = 0;
volatile u64 g_pipeit_poll_hits_output = 0;
volatile u64 g_pipeit_iar_spurious_hidden = 0;
volatile u64 g_pipeit_iar_spurious_output = 0;
volatile u64 g_pipeit_last_iar_hidden = 0;
volatile u64 g_pipeit_last_iar_output = 0;
// Loop-iteration counters: increment EVERY pass through the worker loop.
// loop_iter==1 -> WFI never woke. loop_iter>1 but hits==0 -> WFI wakes but
// IAR always returns spurious (priority blocking? RD pending bit cleared?)
volatile u64 g_pipeit_loop_iter_hidden = 0;
volatile u64 g_pipeit_loop_iter_output = 0;
// Snapshot of GIC PE-side state read from inside the worker on each WFI wake,
// so we can see if PE-visible state explains why IAR returned spurious.
volatile u64 g_pipeit_last_hppir1_hidden = 0;
volatile u64 g_pipeit_last_rpr_hidden    = 0;
volatile u64 g_pipeit_last_ispendr0_hidden = 0;
volatile u64 g_pipeit_last_hppir1_output = 0;
volatile u64 g_pipeit_last_rpr_output    = 0;
volatile u64 g_pipeit_last_ispendr0_output = 0;

// Read ICC_IAR1_EL1 (acknowledges the highest-priority pending Group 1 IRQ
// and returns its INTID; 1023 = spurious / no IRQ pending).
static inline u32 read_iar1(void) {
    u64 v;
    asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v));   // ICC_IAR1_EL1
    return (u32)v;
}
static inline void write_eoir1(u32 intid) {
    asm volatile("msr S3_0_C12_C12_1, %0" :: "r"((u64)intid));   // ICC_EOIR1_EL1
    asm volatile("isb");
}

// Hybrid poll+WFI worker. Pattern:
//   1) Poll IAR1. If a real INTID comes back, dispatch handler + EOI.
//   2) Otherwise enter WFI to sleep until next IRQ assert.
// This works around any failure in the architected IRQ vector path: as long
// as the GIC delivers the SGI to this RD (ispendr0 bit set), IAR1 will
// surface it and we acknowledge manually. If the architected vector ALSO
// fires concurrently, sgi_counters[][] will increment too — both paths are
// idempotent because pipeit_sgi_hidden() consumes only one buffer slot.
// Read GIC PE-side state on a self-core. Used to debug why IAR returns spurious.
static inline u64 read_hppir1(void) {
    u64 v; asm volatile("mrs %0, S3_0_C12_C12_2" : "=r"(v)); return v;  // ICC_HPPIR1_EL1
}
static inline u64 read_rpr(void) {
    u64 v; asm volatile("mrs %0, S3_0_C12_C11_3" : "=r"(v)); return v;  // ICC_RPR_EL1
}

// Lock-free WFE/SEV mailbox workers (Level-2 design).
// SGI on A72 is broken on RK3399 (GICR.ChildrenAsleep stuck), but the WFE/SEV
// event mechanism uses a separate event register, NOT the GIC. SEV is broadcast
// across the system (including across the CCI bridge between A53 and A72).
// Strategy: adaptive spin (a few cycles of cheap polling for low-latency case),
// then WFE to sleep until producer issues SEV. After WFE wakes, re-check seq.
//
// Memory ordering: producer writes data, dmb, increments seq, dc cvac, dsb, sev.
// Consumer: wfe (or polled), dc ivac on seq, read seq, if changed -> dc ivac on
// data, dmb, process. dc ivac is required because A72 cluster does not snoop
// A53 cluster writes (CCI in non-snooping config on RK3399).
//
// Adaptive spin count: ~16 fast polls before WFE. At ~1.5GHz this is ~10ns.
// First several polls hit cached seq value; once producer's dc cvac lands, the
// invalidate-then-read sees the new value with no WFE round-trip. If the wait
// is longer than the spin window, WFE saves power.
// Spin budget tuned for sequential benchmark (frame interval ~12 us).
// Each spin iteration is ~30-50 ns (ivac+dsb_ish on hot line), so 128 iters
// covers ~5-7 us before falling back to WFE. Producer's dc cvac+sev path
// reaches the worker faster than that, so worker stays in spin and avoids
// the cross-cluster WFE wake (~1-3 us).
#define PIPEIT_SPIN_BUDGET 128

void pipeit_worker_idle_hidden(void) {
    pmu_init_local_a72();
    u64 last_seen = 0;
    u32 use_sgi_drain = g_pipeit_use_sgi_a72;
    while (g_pipe && g_pipeit_active) {
        g_pipeit_loop_iter_hidden += 1;
        // Adaptive spin: fast path for back-to-back submissions.
        // ivac fires every other iteration (cheap volatile read in between);
        // ivac is mandatory across CCI to drop A72-side stale copy, but two
        // ivacs per producer beat is wasteful, so we space them out.
        // Bell is NC: just read it. No dc ivac. dsb ish would only matter
        // if there were prior cacheable accesses to order against; the read
        // itself sees fresh DRAM directly.
        u32 spin = PIPEIT_SPIN_BUDGET;
        u64 now;
        for (;;) {
            now = g_pipeit_hidden_pending;
            if (now != last_seen) break;
            if (!g_pipe || !g_pipeit_active) return;
            if (spin == 0) {
                asm volatile("wfe" ::: "memory");
                spin = PIPEIT_SPIN_BUDGET;
                continue;
            }
            spin--;
        }
        last_seen = now;
        g_pipeit_poll_hits_hidden += 1;
        // dispatch_idx is cacheable — invalidate before reading.
        asm volatile("dc ivac, %0" :: "r"(&g_pipe->buffer->dispatch_idx) : "memory");
        asm volatile("dsb ish" ::: "memory");
        asm volatile("dmb ish" ::: "memory");
        pipeit_sgi_hidden(0);
        if (use_sgi_drain) {
            // Drain SGI line opportunistically when RD is really awake.
            u32 intid = read_iar1();
            g_pipeit_last_iar_hidden = intid;
            if (intid < 16) {
                write_eoir1(intid);
            } else if (intid == 1023) {
                g_pipeit_iar_spurious_hidden += 1;
            }
        }
    }
}

void pipeit_worker_idle_output(void) {
    pmu_init_local_a72();
    u64 last_seen = 0;
    u32 use_sgi_drain = g_pipeit_use_sgi_a72;
    while (g_pipe && g_pipeit_active) {
        g_pipeit_loop_iter_output += 1;
        // Output bell is NC. (Note: in the FUSED design this loop is
        // effectively dormant — pipeit_submit_frame never increments
        // g_pipeit_output_pending; output stage runs inline on core 4.)
        u32 spin = PIPEIT_SPIN_BUDGET;
        u64 now;
        for (;;) {
            now = g_pipeit_output_pending;
            if (now != last_seen) break;
            if (!g_pipe || !g_pipeit_active) return;
            if (spin == 0) {
                asm volatile("wfe" ::: "memory");
                spin = PIPEIT_SPIN_BUDGET;
                continue;
            }
            spin--;
        }
        last_seen = now;
        g_pipeit_poll_hits_output += 1;
        asm volatile("dc ivac, %0" :: "r"(&g_pipe->buffer->dispatch_idx) : "memory");
        asm volatile("dsb ish" ::: "memory");
        asm volatile("dmb ish" ::: "memory");
        pipeit_sgi_output(0);
        if (use_sgi_drain) {
            u32 intid = read_iar1();
            g_pipeit_last_iar_output = intid;
            if (intid < 16) {
                write_eoir1(intid);
            } else if (intid == 1023) {
                g_pipeit_iar_spurious_output += 1;
            }
        }
    }
}
