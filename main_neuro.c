#include <stdint.h>
#include "core/types.h"
#include "core/heartbeat.h"
#include "hal/gicv3.h"
#include "hal/cci.h"
#include "core/slab.h"
#include "hal/gmac.h"
#include "neuro/neuro_sync.h"
#include "neuro/neuro_parallel.h"
#include "neuro/pipeit.h"
#include "neuro/telemetry.h"
#include "neuro/weight_validation.h"
#include "neuro/adaptive_scheduler.h"
#include "core/smp.h"
#include "core/log.h"
#include "core/workqueue.h"
#include "mmu.h"
#include "hal/net.h"
#include "hal/pmu.h"
#include "hal/cru.h"
#include "hal/mali.h"
#include "hal/mali_jm.h"
#include "hal/mali_mmu.h"
#include "hal/mali_compute.h"
#include "hal/tsadc.h"
#include "core/thermal_guard.h"
#include "core/wcet.h"
#include "hal/hexo_l2.h"
#include "hal/hexo_ptp.h"
#include "hal/hexo_offload.h"
#include "neuro/gossip.h"
#include "core/peer_table.h"

extern volatile u32 gmac_rx_pending;

#define UART2_BASE 0xFF1A0000
#define UART_THR   0x00
#define UART_USR   0x7C

uart_t console;
static neuro_sync_t neural_arbitrator;

// Phase 2: Pipe-it pipeline instance + double buffer
static pipeit_t g_pipeit;
static pipe_buffer_t __attribute__((aligned(64))) g_pipe_buffer;

// Phase 4.2: Thermal guard with workload throttling
static thermal_guard_t g_thermal_guard;

// Phase 4.3: WCET measurement regions
static wcet_region_t g_wcet_inference;
static wcet_region_t g_wcet_pipeit;

// Phase 5.4: Gossip federated learning instance
static gossip_t g_gossip;

// Phase 6.3: Beacon broadcast period (1s at 24MHz)
#define BEACON_PERIOD_CYC (24000000ULL)
static u64 g_next_beacon_cyc = 0;

static telemetry_collector_t telemetry;
static adaptive_scheduler_t adaptive_sched;
static inference_result_t last_inference_result;
static telemetry_t runtime_telemetry_snapshot;
static u64 runtime_last_tick_cycles;
static u32 runtime_loop_jitter_percent;
static bool runtime_last_offload_state;

// PMU Phase 0: Baseline measurement tracking
static pmu_snapshot_t pmu_snap_start, pmu_snap_end, pmu_snap_delta;
static u64 pmu_inference_count = 0;
static u64 pmu_total_cycles = 0;
static u64 pmu_total_l1_misses = 0;
static u64 pmu_total_l2_misses = 0;
#define PMU_BASELINE_COUNT 10000

// === A72 baseline isolation (Phase 0.5) ===
// Boot CPU is A53. To compare cluster effect with the SAME NEON code path,
// we dispatch an identical baseline loop to A72 core 4 via the workqueue,
// then read the results back through this shared struct (Normal cacheable
// + dmb-ish; CCI-500 keeps the line coherent across clusters).
typedef struct {
    volatile u32 done_flag;
    u32 _pad;
    u64 mpidr;
    // Clock raise diagnostics: target freq requested vs cru_set_a72_freq_mhz return code.
    i64 clk_set_ret;       // 0 = OK, -1 = lock timeout, -2 = unsupported, -3 = skipped
    i64 clk_first_ret;     // Return code from first (highest) OPP attempt
    u64 clk_target_mhz;
    u64 clk_applied_mhz;
    u64 clk_diag_rk808_id1;
    u64 clk_diag_i2c_status;
    u64 avg_cycles, min_cycles, max_cycles;
    u64 avg_inst, avg_l1m, avg_l2m, avg_bus, avg_br;
    u64 avg_asimd;         // ASIMD_SPEC: NEON ops speculatively executed (A72)
    u64 avg_ticks, min_ticks, max_ticks, var_ticks;
    u64 avg_ns, elapsed_us;
    u64 ipc_x1000, cpu_mhz;
    u64 cpu_mhz_actual; // Phase 0.2: PMCCNTR/CNTPCT 1ms window; 0 if PMU off
    u64 ddr_mbps;
} __attribute__((aligned(64))) baseline_xfer_t;

static baseline_xfer_t g_a72_baseline __attribute__((aligned(64)));
static telemetry_t     g_a72_warmup_tel;

// Runs on core 1 (A53) via work queue: one neural inference cycle per ICMP ping.
// Phase 4.3: WCET-instrumented for empirical latency tracking
static void neuro_infer_worker(u64 arg) {
    (void)arg;
    wcet_begin(&g_wcet_inference);
    adaptive_inference(&adaptive_sched, &telemetry.current, &last_inference_result);
    wcet_end(&g_wcet_inference);
}

static inline u64 read_cntpct(void) {
    u64 v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

#define RK_SIP_GICR_WAKER_GET_64 0xC20000A2ULL
#define RK_SIP_GICR_WAKE_TRY_64  0xC20000A3ULL

typedef struct {
    u64 status;
    u64 waker;
} sip_gicr_get_t;

typedef struct {
    u64 status_flags;
    u64 before;
    u64 after;
} sip_gicr_wake_try_t;

static sip_gicr_get_t sip_gicr_waker_get(u64 core)
{
    sip_gicr_get_t out;
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
    out.status = x0;
    out.waker = x1;
    return out;
}

static sip_gicr_wake_try_t sip_gicr_wake_try(u64 core)
{
    sip_gicr_wake_try_t out;
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
    out.status_flags = x0;
    out.before = x1;
    out.after = x2;
    return out;
}

// Runs on A72 core 4 via wq_dispatch. Mirrors the A53 baseline loop in kmain
// so the only delta vs v2 is "which cluster executed". PMU registers are
// banked per-core so we must call pmu_init_local() before taking snapshots.
static void baseline_runner_a72(u64 arg) {
    (void)arg;

    // Source-backed deterministic OPP ladder:
    // try the target first, then fall back to known-safe lower OPPs without
    // ad-hoc retuning.
    static const u32 a72_opp_ladder[] = {1608u, 1416u, 1200u, 1008u, 816u};

    g_a72_baseline.clk_target_mhz = 1608;
    g_a72_baseline.clk_applied_mhz = 0;
    g_a72_baseline.clk_first_ret = -3;
    g_a72_baseline.clk_diag_rk808_id1 = 0xFF;
    g_a72_baseline.clk_diag_i2c_status = 0;

    i64 last_ret = -3;
    for (u32 i = 0; i < (sizeof(a72_opp_ladder) / sizeof(a72_opp_ladder[0])); i++) {
        i64 ret = (i64)cru_set_a72_freq_mhz(a72_opp_ladder[i]);
        if (i == 0) {
            g_a72_baseline.clk_first_ret = ret;
            if (ret < 0) {
                u32 rk808_id1 = 0xFF;
                u32 i2c_status = 0;
                cru_get_last_pmic_diag(&rk808_id1, &i2c_status,
                                       0, 0, 0, 0, 0, 0, 0, 0);
                g_a72_baseline.clk_diag_rk808_id1 = rk808_id1;
                g_a72_baseline.clk_diag_i2c_status = i2c_status;
            }
        }
        last_ret = ret;
        if (ret == 0) {
            g_a72_baseline.clk_applied_mhz = a72_opp_ladder[i];
            break;
        }
    }
    g_a72_baseline.clk_set_ret = (g_a72_baseline.clk_applied_mhz != 0) ? 0 : last_ret;
    asm volatile("dmb ish" ::: "memory");

    // Per-core PMU setup (A72 has its own PMCR/CNTENSET/PMCCFILTR/MDCR_EL2).
    // Use A72-specific event list with ASIMD_SPEC for NEON profiling.
    pmu_init_local_a72();
    
    pmu_snapshot_t b_start, b_end, b_delta;
    u64 t_min = ~0ULL, t_max = 0, t_sum = 0, t_sum_sq = 0;
    u64 cyc_min = ~0ULL, cyc_max = 0, cyc_sum = 0;
    u64 inst_sum = 0, l1m_sum = 0, l2m_sum = 0, bus_sum = 0, br_sum = 0;
    u64 asimd_sum = 0;
    
    inference_result_t r;

    // Phase 1.4: warmup pass — populate L1i (NEON hidden+output code) and
    // L1d (weight rows w1/w2, bias, hidden buffer) on this PE before the
    // measured loop begins. Without warmup the first iteration eats the
    // cold-cache fill penalty (observed: max=1429 vs avg=228 on A72, 6.3x).
    // 32 iterations is enough to fully populate 32KB L1i/L1d for the 6->8->4
    // network (~1.5KB code + ~256B data). Results are discarded.
    for (u32 wu = 0; wu < 32u; wu++) {
        adaptive_inference_a72(&adaptive_sched, &g_a72_warmup_tel, &r);
    }
    asm volatile("dsb ish; isb" ::: "memory");

    u64 t0 = read_cntpct();
    for (u32 it = 0; it < PMU_BASELINE_COUNT; it++) {
        // Phase 1.3 (ADL-016): mask IRQ around the measurement window so SGI
        // delivery (timer / pipeit fallback) cannot inflate per-iter cycles.
        // A72 baseline runs locally on core 4; no work-queue dispatch inside
        // adaptive_inference_a72(), so masking is safe here.
        u64 daif_save;
        pmu_meas_critical_enter(&daif_save);
        u64 ts0 = read_cntpct();
        pmu_take_snapshot(&b_start);
        adaptive_inference_a72(&adaptive_sched, &g_a72_warmup_tel, &r);
        pmu_take_snapshot(&b_end);
        u64 dt = read_cntpct() - ts0;
        pmu_meas_critical_exit(daif_save);
        pmu_calc_delta(&b_start, &b_end, &b_delta);
        if (dt < t_min) t_min = dt;
        if (dt > t_max) t_max = dt;
        t_sum    += dt;
        t_sum_sq += dt * dt;
        if (b_delta.cycle_count < cyc_min) cyc_min = b_delta.cycle_count;
        if (b_delta.cycle_count > cyc_max) cyc_max = b_delta.cycle_count;
        cyc_sum  += b_delta.cycle_count;
        inst_sum += b_delta.instructions;
        l1m_sum  += b_delta.l1d_cache_refill;
        l2m_sum  += b_delta.l2d_cache_refill;
        bus_sum  += b_delta.bus_cycles;
        br_sum   += b_delta.branch_mispred;
        asimd_sum += b_delta.asimd_spec;
    }
    u64 elapsed = read_cntpct() - t0;
    
    u64 avg_ticks  = t_sum / PMU_BASELINE_COUNT;
    u64 avg_cycles = cyc_sum / PMU_BASELINE_COUNT;
    u64 avg_inst   = inst_sum / PMU_BASELINE_COUNT;
    u64 avg_ns     = (t_sum * 125) / (PMU_BASELINE_COUNT * 3);
    u64 mean_sq    = avg_ticks * avg_ticks;
    u64 e_x_sq     = t_sum_sq / PMU_BASELINE_COUNT;
    u64 var_ticks  = (e_x_sq > mean_sq) ? (e_x_sq - mean_sq) : 0;
    
    // DDR Bandwidth approximation: requires BUS_CYCLES on counter 4.
    // A72 uses L1D_CACHE on counter 4 instead, so DDR calc is invalid → report 0.
    // (A53 baseline still uses BUS_CYCLES and gets correct ddr_mbps.)
    u64 ddr_mbps = 0;
    
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    
    g_a72_baseline.mpidr      = mpidr & 0xFFFFFFu;
    g_a72_baseline.avg_cycles = avg_cycles;
    g_a72_baseline.min_cycles = cyc_min;
    g_a72_baseline.max_cycles = cyc_max;
    g_a72_baseline.avg_inst   = avg_inst;
    g_a72_baseline.avg_l1m    = l1m_sum / PMU_BASELINE_COUNT;
    g_a72_baseline.avg_l2m    = l2m_sum / PMU_BASELINE_COUNT;
    g_a72_baseline.avg_bus    = bus_sum / PMU_BASELINE_COUNT;
    g_a72_baseline.avg_br     = br_sum / PMU_BASELINE_COUNT;
    g_a72_baseline.avg_asimd  = asimd_sum / PMU_BASELINE_COUNT;
    g_a72_baseline.avg_ticks  = avg_ticks;
    g_a72_baseline.ddr_mbps   = ddr_mbps;
    g_a72_baseline.min_ticks  = t_min;
    g_a72_baseline.max_ticks  = t_max;
    g_a72_baseline.var_ticks  = var_ticks;
    g_a72_baseline.avg_ns     = avg_ns;
    g_a72_baseline.elapsed_us = elapsed / 24;
    g_a72_baseline.ipc_x1000  = avg_cycles ? (avg_inst * 1000) / avg_cycles : 0;
    g_a72_baseline.cpu_mhz    = avg_ns ? (avg_cycles * 1000) / avg_ns : 0;
    // Phase 0.2 (ADL: cpu_mhz from per-iter avg_ticks is noisy at ~4-tick
    // windows). Sample over a 1 ms wall-time window via PMCCNTR/CNTPCT —
    // independent of inference geometry, sub-1% accurate at any OPP.
    g_a72_baseline.cpu_mhz_actual = pmu_measure_cpu_freq_mhz(1000u);

    // Ensure all writes visible to boot CPU before flag flips.
    asm volatile("dmb ish" ::: "memory");
    g_a72_baseline.done_flag = 1;
}

// Raw UART write path for early-debug checkpoints (bypasses uart_puts/uart_putc).
static inline void dbg_putc_raw(char c) {
    volatile u32 *uart = (volatile u32 *)(uintptr_t)UART2_BASE;
    while (!(uart[UART_USR >> 2] & 2u)) {
        asm volatile("yield");
    }
    uart[UART_THR >> 2] = (u32)c;
}

static void runtime_update_loop_jitter(u64 now) {
    if (runtime_last_tick_cycles != 0) {
        u64 delta = now - runtime_last_tick_cycles;
        i64 deviation = (i64)delta - (i64)HEARTBEAT_CYCLES_24MHZ;
        if (deviation < 0) {
            deviation = -deviation;
        }
        runtime_loop_jitter_percent = (u32)((deviation * 100) / HEARTBEAT_CYCLES_24MHZ);
        adaptive_scheduler_update(&adaptive_sched, runtime_loop_jitter_percent);
    }
    runtime_last_tick_cycles = now;
}

static void runtime_print_summary(void) {
    uart_puts(&console, "[RUNTIME] mode=");
    uart_puts(&console, runtime_last_offload_state ? "offload" : "core0");
    uart_puts(&console, " jitter=0x");
    uart_put_hex(&console, runtime_loop_jitter_percent);
    uart_puts(&console, " cpu=0x");
    uart_put_hex(&console, runtime_telemetry_snapshot.cpu_load);
    uart_puts(&console, " mem=0x");
    uart_put_hex(&console, runtime_telemetry_snapshot.memory_pressure);
    uart_puts(&console, " pkt=0x");
    uart_put_hex(&console, runtime_telemetry_snapshot.packet_rate);
    uart_puts(&console, " nodes=0x");
    uart_put_hex(&console, runtime_telemetry_snapshot.node_count);
    uart_puts(&console, "\r\n");
}

static u32 runtime_last_worker_core = 0;
static bool use_parallel_inference = true;
static bool parallel_mode_active = false;

static void runtime_run_inference(bool prefer_offload) {
    if (telemetry_collect(&telemetry, &runtime_telemetry_snapshot) != OK) {
        return;
    }

    // PMU Phase 0: Take snapshot before inference
    if (pmu_inference_count < PMU_BASELINE_COUNT) {
        pmu_take_snapshot(&pmu_snap_start);
    }

    inference_result_t result;
    result_t res;

    // Try parallel 6-core inference first if all cores available
    u32 online = smp_get_online_count();
    static u32 debug_shown = 0;
    if (!debug_shown) {
        uart_puts(&console, "[DEBUG] online_cores=");
        uart_put_hex(&console, online);
        uart_puts(&console, " use_parallel=");
        uart_put_hex(&console, use_parallel_inference ? 1 : 0);
        uart_puts(&console, "\r\n");
        debug_shown = 1;
    }
    if (use_parallel_inference && online >= 6) {
        res = neuro_parallel_inference_sync(&runtime_telemetry_snapshot, &result);
        if (res == OK) {
            last_inference_result = result;
            runtime_last_offload_state = true;
            if (!parallel_mode_active) {
                uart_puts(&console, "[RUNTIME] inference -> 6-core parallel (cores=");
                uart_put_hex(&console, online);
                uart_puts(&console, ")\r\n");
                parallel_mode_active = true;
            }
            return;
        }
        // Fallback to single-core if parallel fails
        uart_puts(&console, "[DEBUG] parallel failed, res=");
        uart_put_hex(&console, (u64)res);
        uart_puts(&console, "\r\n");
        use_parallel_inference = false;
        parallel_mode_active = false;
    }

    // Fallback: single-core via work queue
    u32 worker_core = 0;
    bool offloaded = false;

    if (prefer_offload && smp_get_online_count() > 1) {
        worker_core = wq_dispatch_any(neuro_infer_worker, 0);
        offloaded = (worker_core != 0);
    }

    if (!offloaded) {
        neuro_infer_worker(0);
    }

    if (offloaded && worker_core != runtime_last_worker_core) {
        uart_puts(&console, "[RUNTIME] inference -> core ");
        uart_put_hex(&console, worker_core);
        uart_puts(&console, "\r\n");
        runtime_last_worker_core = worker_core;
    }

    runtime_last_offload_state = offloaded;

    // PMU Phase 0: Take snapshot after inference and accumulate stats
    if (pmu_inference_count < PMU_BASELINE_COUNT) {
        pmu_take_snapshot(&pmu_snap_end);
        pmu_calc_delta(&pmu_snap_start, &pmu_snap_end, &pmu_snap_delta);
        pmu_total_cycles += pmu_snap_delta.cycle_count;
        pmu_total_l1_misses += pmu_snap_delta.l1d_cache_refill;
        pmu_total_l2_misses += pmu_snap_delta.l2d_cache_refill;
        pmu_inference_count++;

        // Print progress every 1000 inferences
        if ((pmu_inference_count % 1000) == 0) {
            uart_puts(&console, "[PMU] baseline_progress=");
            uart_put_hex(&console, pmu_inference_count);
            uart_puts(&console, "/");
            uart_put_hex(&console, PMU_BASELINE_COUNT);
            uart_puts(&console, " avg_cycles=");
            uart_put_hex(&console, pmu_total_cycles / pmu_inference_count);
            uart_puts(&console, "\r\n");
        }

        // Print final summary at 10000
        if (pmu_inference_count == PMU_BASELINE_COUNT) {
            uart_puts(&console, "\r\n[PMU] BASELINE COMPLETE\r\n");
            uart_puts(&console, "[PMU] total_inferences=");
            uart_put_hex(&console, PMU_BASELINE_COUNT);
            uart_puts(&console, "\r\n");
            uart_puts(&console, "[PMU] avg_cycles=");
            uart_put_hex(&console, pmu_total_cycles / PMU_BASELINE_COUNT);
            uart_puts(&console, "\r\n");
            uart_puts(&console, "[PMU] avg_l1_misses=");
            uart_put_hex(&console, pmu_total_l1_misses / PMU_BASELINE_COUNT);
            uart_puts(&console, "\r\n");
            uart_puts(&console, "[PMU] avg_l2_misses=");
            uart_put_hex(&console, pmu_total_l2_misses / PMU_BASELINE_COUNT);
            uart_puts(&console, "\r\n");
        }
    }
}

static void print_banner(void) {
    uart_puts(&console, "\r\n");
    uart_puts(&console, "========================================\r\n");
    uart_puts(&console, "  H-Exo Omni-Core v1.0\r\n");
    uart_puts(&console, "  Neural Arbitrator (Neuro-Sync) Active\r\n");
    uart_puts(&console, "========================================\r\n");
    uart_puts(&console, "\r\n");
}


void kmain(void) {
    u64 boot_start = read_cntpct();

    // Initialize UART
    uart_config_t uart_cfg = {
        .base_addr = UART2_BASE,
        .baud_rate = 1500000,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = 0,
        .fifo_depth = 16
    };
    uart_init(&console, &uart_cfg);

    // v20 DIAGNOSTIC: Read I2C0 RK3x registers from A53 core (core 0) BEFORE any init
    {
        volatile u32 *i2c0 = (volatile u32 *)(uintptr_t)0xFF3C0000UL;
        uart_puts(&console, "[I2C0_DIAG_A53] CON=0x");
        uart_put_hex(&console, i2c0[0x00 >> 2]);
        uart_puts(&console, " CLKDIV=0x");
        uart_put_hex(&console, i2c0[0x04 >> 2]);
        uart_puts(&console, " MRXADDR=0x");
        uart_put_hex(&console, i2c0[0x08 >> 2]);
        uart_puts(&console, " MRXRADDR=0x");
        uart_put_hex(&console, i2c0[0x0C >> 2]);
        uart_puts(&console, " MTXCNT=0x");
        uart_put_hex(&console, i2c0[0x10 >> 2]);
        uart_puts(&console, " MRXCNT=0x");
        uart_put_hex(&console, i2c0[0x14 >> 2]);
        uart_puts(&console, " IPD=0x");
        uart_put_hex(&console, i2c0[0x1C >> 2]);
        uart_puts(&console, " FCNT=0x");
        uart_put_hex(&console, i2c0[0x20 >> 2]);
        uart_puts(&console, "\r\n");
    }

    // Switch TTBR0_EL2 to our tables: DRAM=Normal WB, MMIO=Device-nGnRnE
    mmu_init();
    mmu_enable();
    LOG_OK("MMU: EL2 identity map active");

    // Refine first-1GB mapping: split into 2MB blocks and mark PA 0x10000000
    // as Normal Non-Cacheable so cross-cluster bell variables (pipeit pipeline
    // doorbells) bypass cluster L1/L2 caches and the slow CCI-500 cache
    // maintenance path. See mmu_nc.c.
    mmu_install_nc_region();
    LOG_OK("MMU: NC region installed at 0x10000000 (2MB)");

    // 1. Initialize Slab Allocator
    slab_init();
    LOG_OK("Slab: Initialized (512KB Heap)");

    // 1.5 CCI-500 coherency.
    // NOTE: cci500_enable() is a deliberate NO-OP. NS EL2 writes to CCI slave
    // interfaces create CHANGE_PENDING transactions that never complete (the
    // CCI is owned by TF-A secure world); the stuck PENDING blocked TF-A's
    // cci_enable_snoop_dvm_reqs() during pwr_domain_on_finish -> watchdog ->
    // SoC reset (docs/rk3399/smp-bringup.md, ADL-005). TF-A enables
    // snoop+DVM for both clusters, so an "OK" return means "nothing to do",
    // NOT "we enabled it". Report the measured state instead of claiming it.
    if (cci500_enable() == OK) {
        u32 cci_c = 0, cci_s = 0, cci_a53 = 0, cci_a72 = 0;
        cci500_diag_read(&cci_c, &cci_s, &cci_a53, &cci_a72);
        uart_puts(&console, "[OK] CCI-500: TF-A managed (no NS writes) a53_snoop=0x");
        uart_put_hex(&console, cci_a53);
        uart_puts(&console, " a72_snoop=0x");
        uart_put_hex(&console, cci_a72);
        uart_puts(&console, "\r\n");
    } else {
        LOG_WARN("CCI-500: enable timeout");
    }
    
    // Read-only diagnostic: dump actual CCI snoop state. If A53/A72 snoop_ctrl
    // bits are 0, TF-A did NOT enable snoop and we have a path to fix.
    {
        u32 cci_ctrl = 0, cci_status = 0, sn_a53 = 0, sn_a72 = 0;
        cci500_diag_read(&cci_ctrl, &cci_status, &sn_a53, &sn_a72);
        uart_puts(&console, "[CCI_DIAG] ctrl=0x");    uart_put_hex(&console, cci_ctrl);
        uart_puts(&console, " status=0x");            uart_put_hex(&console, cci_status);
        uart_puts(&console, " a53_snoop=0x");         uart_put_hex(&console, sn_a53);
        uart_puts(&console, " a72_snoop=0x");         uart_put_hex(&console, sn_a72);
        uart_puts(&console, "\r\n");
    }

    // 2. SMP: pre-wake all GIC redistributors BEFORE PSCI CPU_ON.
    //    BL31 parks secondary cores in WFI and sets GICR_WAKER.ProcessorSleep=1 for them.
    //    When PSCI CPU_ON sends the wake SGI, if ProcessorSleep=1 the redistributor
    //    silently drops it and the core never leaves WFI.
    //    Clearing ProcessorSleep from core 0 (safe MMIO write) before CPU_ON ensures
    //    the SGI is delivered the moment BL31 sends it.
    // Log GICR_WAKER after pre-wake to confirm state (ProcessorSleep=bit1, ChildrenAsleep=bit2)
    {
        uart_puts(&console, "[GIC] GICR_WAKER before PSCI:");
        for (u32 cpu = 0; cpu < 4; cpu++) {
            volatile u32 *waker = (volatile u32*)(0xFEF00014UL + (uintptr_t)cpu * 0x20000);
            uart_puts(&console, " C");
            uart_put_hex(&console, cpu);
            uart_puts(&console, "=");
            uart_put_hex(&console, *waker);
        }
        uart_puts(&console, "\r\n");
    }
    gicv3_prewake_redistributors();
    {
        u32 ready_mask = 0;
        u32 stuck_mask = 0;
        uart_puts(&console, "[GIC] GICR_WAKER after prewake:");
        for (u32 cpu = 0; cpu < 6; cpu++) {
            volatile u32 *waker = (volatile u32*)(0xFEF00014UL + (uintptr_t)cpu * 0x20000);
            u32 w = *waker;
            if ((w & (1u << 2)) == 0u) {
                ready_mask |= (1u << cpu);
            } else {
                stuck_mask |= (1u << cpu);
            }
            uart_puts(&console, " C");
            uart_put_hex(&console, cpu);
            uart_puts(&console, "=");
            uart_put_hex(&console, w);
        }
        uart_puts(&console, " ready=0x");
        uart_put_hex(&console, ready_mask);
        uart_puts(&console, " stuck=0x");
        uart_put_hex(&console, stuck_mask);
        uart_puts(&console, "\r\n");
    }

    // ADL-014 (Fix #2 / Plan v3.2): set A53 boot-time performance OPP.
    // Default boot leaves cluster L (A53) on the lowest LPLL OPP (~514 MHz on
    // NanoPi M4), which biases every downstream baseline by ~2.7x. Lift to a
    // safe 1008 MHz that does not require a VDD_CPU_L bump (RK808 BUCK1
    // default ~0.9 V supports 1008 MHz per RK3399 reference OPP table).
    // 1200/1416 MHz require explicit PMIC bump and are intentionally NOT used
    // here to keep boot path unconditional and deadlock-free.
    {
        int a53_ret = cru_set_a53_freq_mhz(1008u);
        uart_puts(&console, "[A53_OPP] target_mhz=0x");
        uart_put_hex(&console, 1008u);
        uart_puts(&console, " ret=0x");
        uart_put_hex(&console, (u64)(i64)a53_ret);
        uart_puts(&console, "\r\n");
    }

    smp_init();

    // Post-PSCI re-wake for A72 redistributors: on some RK3399 boots,
    // cores 4/5 can come online while GICR_WAKER.ChildrenAsleep remains set.
    // This pass forces ProcessorSleep=0 again after CPU_ON completion.
    {
        u32 w4_before = gicv3_read_waker(4);
        u32 w5_before = gicv3_read_waker(5);
        u32 w4_after = gicv3_force_wake_core(4, 8);
        u32 w5_after = gicv3_force_wake_core(5, 8);
        uart_puts(&console, "[GIC] post-SMP re-wake w4:");
        uart_put_hex(&console, w4_before);
        uart_puts(&console, "->");
        uart_put_hex(&console, w4_after);
        uart_puts(&console, " w5:");
        uart_put_hex(&console, w5_before);
        uart_puts(&console, "->");
        uart_put_hex(&console, w5_after);
        uart_puts(&console, "\r\n");
    }

    // Reverted to running AFTER smp_init(). With gicd_wait_for_rwp() in place,
    // calling gicv3_init() ahead of smp_init() hangs the board: it prints
    // "GICv3: Interrupt Controller Ready" and then stops dead, with no UART
    // response and no network, because the runtime console handlers are
    // registered far later in kmain. Verified on hardware 2026-10-03. The three
    // IHI 0069G s2.3.3 / s12.9.4 corrections inside gicv3_init() are kept - they
    // stand on their own and are tested in this position.
    //
    // Core 0's own gicv3_init_cpu_iface() must also stay after smp_init(): it
    // depends on state smp_init() establishes, and moving it earlier hangs too.
    //
    // NOTE (2026-10-03, regression found): an earlier revert of the reorder
    // experiment accidentally deleted the gicv3_init() CALL, leaving only this
    // comment. All builds since ran with the Distributor left in BL31's state.
    // gicv3_init() must run here - after smp_init, before per-core cpu-iface.
    if (gicv3_init() == OK) {
        LOG_OK("GICv3: Interrupt Controller Ready");
    } else {
        LOG_ERR("GICv3: Initialization Failed");
    }
    gicv3_init_cpu_iface();
    uart_puts(&console, "[OK] SMP: ");
    uart_put_hex(&console, smp_get_online_count());
    uart_puts(&console, " cores online\r\n");
    {
        register u64 x0 asm("x0") = 0xC20000A0UL;
        register u64 x1 asm("x1") = 0;
        register u64 x2 asm("x2") = 0;
        register u64 x3 asm("x3") = 0;
        asm volatile("smc #0"
                     : "+r"(x0), "+r"(x1)
                     : "r"(x2), "r"(x3)
                     : "memory",
                       "x4", "x5", "x6", "x7", "x8", "x9",
                       "x10", "x11", "x12", "x13", "x14", "x15",
                       "x16", "x17");
        uart_puts(&console, "[FW_DIAG] magic=0x");
        uart_put_hex(&console, x0);
        uart_puts(&console, " version=0x");
        uart_put_hex(&console, x1);
        uart_puts(&console, "\r\n");
    }
    
    // Idea #3: report A72 CPUECTLR.SMPEN as probed via SiP SMC RK_SIP_SMPEN_GET
    // by smp_secondary_main on cores 4/5. See docs/tfa_smpen_diag_patch.md.
    //   value 0           -> SMPEN clear (root cause confirmed!)
    //   value 1           -> SMPEN set   (problem is elsewhere)
    //   value 0xFFFFFFFF  -> SMC_UNK     (TF-A patch not in firmware)
    //   value 0xCAFE000N  -> probe entered but SMC didn't return
    //   value 0xDEADBEEF  -> probe never executed (timeout below)
    {
        extern volatile u64 g_smpen_diag[6];
        // Spin-wait up to ~10ms for both A72 cores to update their slot.
        // PSCI returning ONLINE only guarantees the PE is executing code,
        // not that it's reached our probe. Cores might still be in the
        // trampoline or early in smp_secondary_main when we get here.
        for (u32 wait_iter = 0; wait_iter < 1000000; wait_iter++) {
            asm volatile("dc ivac, %0" :: "r"(&g_smpen_diag[4]) : "memory");
            asm volatile("dc ivac, %0" :: "r"(&g_smpen_diag[5]) : "memory");
            asm volatile("dsb sy" ::: "memory");
            if (g_smpen_diag[4] != 0xDEADBEEFUL &&
                g_smpen_diag[5] != 0xDEADBEEFUL) break;
            asm volatile("yield");
        }
        // Final ivac+dsb before reading.
        asm volatile("dc ivac, %0" :: "r"(&g_smpen_diag[4]) : "memory");
        asm volatile("dc ivac, %0" :: "r"(&g_smpen_diag[5]) : "memory");
        asm volatile("dsb sy" ::: "memory");
        uart_puts(&console, "[SMPEN_DIAG] core4=0x");
        uart_put_hex(&console, g_smpen_diag[4]);
        uart_puts(&console, " core5=0x");
        uart_put_hex(&console, g_smpen_diag[5]);
        uart_puts(&console, "\r\n");
    }

    // Idea #6: L2ACTLR_EL1 of the A72 cluster's L2 block, read from EL3.
    // dec bit0 is FORCE_L2_GIC_TIMER_RCG_CLK_ACTIVE - the clock-gated GIC timer
    // subdomain. If it is off, the redistributor latches a pending bit but the
    // CPU interface never observes it, which is the measured symptom.
    // raw == DEADBEEF means the probe never ran on that core (by design: the
    // S3_1_C15_C0_0 encoding is A72 specific and must not run on an A53).
    {
        extern volatile u64 g_l2actlr_diag[6][2];
        for (u32 ci = 0; ci < 6u; ci++) {
            u64 raw = g_l2actlr_diag[ci][0];
            u64 dec = g_l2actlr_diag[ci][1];
            if (raw == 0xDEADBEEFDEADBEEFUL) continue;
            uart_puts(&console, "[L2ACTLR_DIAG] core=0x");
            uart_put_hex(&console, ci);
            uart_puts(&console, " raw=0x"); uart_put_hex(&console, raw);
            uart_puts(&console, " dec=0x"); uart_put_hex(&console, dec);
            uart_puts(&console, " gic_tmr_clk=");  uart_put_hex(&console, dec & 1u);
            uart_puts(&console, " l2_logic_clk="); uart_put_hex(&console, (dec >> 1) & 1u);
            uart_puts(&console, " tag_bank_clk="); uart_put_hex(&console, (dec >> 2) & 1u);
            uart_puts(&console, " dvm_cmo_dis="); uart_put_hex(&console, (dec >> 3) & 1u);
            uart_puts(&console, " no_dvm_sync="); uart_put_hex(&console, (dec >> 4) & 1u);
            uart_puts(&console, " ace_sh_dis=");  uart_put_hex(&console, (dec >> 5) & 1u);
            uart_puts(&console, " haz_timeout="); uart_put_hex(&console, (dec >> 6) & 1u);
            uart_puts(&console, " uniq_clean=");  uart_put_hex(&console, (dec >> 7) & 1u);
            // Which step of gicv3_init_cpu_iface() this PE reached; 99 = returned
            // normally. Appended here rather than printed separately so it rides
            // out on a line that is already proven to reach the console.
            {
                extern volatile u64 gicv3_cpu_iface_stage[6];
                asm volatile("dc ivac, %0" :: "r"(&gicv3_cpu_iface_stage[0]) : "memory");
                asm volatile("dc ivac, %0" :: "r"(&gicv3_cpu_iface_stage[1]) : "memory");
                asm volatile("dc ivac, %0" :: "r"(&gicv3_cpu_iface_stage[2]) : "memory");
                asm volatile("dc ivac, %0" :: "r"(&gicv3_cpu_iface_stage[3]) : "memory");
                asm volatile("dc ivac, %0" :: "r"(&gicv3_cpu_iface_stage[4]) : "memory");
                asm volatile("dc ivac, %0" :: "r"(&gicv3_cpu_iface_stage[5]) : "memory");
                asm volatile("dsb sy" ::: "memory");
                uart_puts(&console, " cpuiface_stage=");
                for (u32 cs = 0; cs < 6u; cs++) {
                    if (cs) uart_puts(&console, "/");
                    uart_put_hex(&console, gicv3_cpu_iface_stage[cs]);
                }

    // Two-moment GIC snapshot comparison. Every software-visible register has
    // compared identical between the working A53s and the dead A72s, so the
    // useful question is no longer "what differs between clusters" but "what
    // changed on the A72 between the moment its CPU interface was initialised
    // and the moment it went back to its idle loop". Moment A is taken by the PE
    // itself right after gicv3_init_cpu_iface() returns; moment B on its first
    // idle-loop pass. The A53 columns are the control.
    {
        static const char *slot_name[GICV3_SNAP_SLOTS] = {
            "GICR_CTLR","GICR_IIDR","GICR_TYPER","GICR_0C","GICR_10","GICR_WAKER",
            "IGROUPR0","ISENABLER0","ICENABLER0","ISPENDR0","ICPENDR0","IPRIORITYR0",
            "ICFGR0","IGRPMODR0","NSACR","GICD_CTLR","GICD_TYPER","GICD_IGROUPR",
            "ICC_SRE_EL1","ICC_PMR_EL1","ICC_IGRPEN0","ICC_IGRPEN1","ICC_HPPIR1",
            "ICC_RPR_EL1","ICC_AP1R0_EL1","DAIF","SCTLR_EL2","HCR_EL2","CNTFRQ",
            "ENDMARK"
        };
        for (u32 ci = 0; ci < 6u; ci++) {
            for (u32 k = 0; k < GICV3_SNAP_SLOTS; k++) {
                asm volatile("dc ivac, %0" :: "r"(&g_gic_snap[ci][0][k]) : "memory");
            }
            for (u32 k = 0; k < GICV3_SNAP_SLOTS; k++) {
                for (u32 m = 0; m < 2u; m++) {
                    asm volatile("dc ivac, %0" :: "r"(&g_gic_snap[ci][m][k]) : "memory");
                }
            }
        }
        asm volatile("dsb sy" ::: "memory");
        // Moment B, taken by core 0 right here, memory-mapped registers only.
        // The A72 cores deliberately do NOT run this code - see
        // gicv3_take_snapshot_mmio(). An MMIO or system-register read that
        // faults on them wedges them before the runtime console exists.
        for (u32 ci = 0; ci < 6u; ci++) {
            gicv3_take_snapshot_mmio(ci, 1u);
        }
        uart_puts(&console, "\r\n[GICSNAP] A = after gicv3_init_cpu_iface, B = first idle pass\r\n");
        for (u32 ci = 0; ci < 6u; ci++) {
            u32 ndiff = 0;
            for (u32 k = 0; k < GICV3_SNAP_SLOTS; k++) {
                if (g_gic_snap[ci][0][k] != g_gic_snap[ci][1][k]) ndiff++;
            }
            uart_puts(&console, "[GICSNAP] core");
            uart_put_hex(&console, ci);
            uart_puts(&console, " A_vs_B_diff=");
            uart_put_hex(&console, ndiff);
            uart_puts(&console, " B_taken=");
            uart_put_hex(&console, (u64)g_gic_snap_b_done[ci]);
            uart_puts(&console, "\r\n");
            if (ndiff == 0u) continue;
            for (u32 k = 0; k < GICV3_SNAP_SLOTS; k++) {
                if (g_gic_snap[ci][0][k] == g_gic_snap[ci][1][k]) continue;
                uart_puts(&console, "   ");
                uart_puts(&console, slot_name[k]);
                uart_puts(&console, " A=0x"); uart_put_hex(&console, g_gic_snap[ci][0][k]);
                uart_puts(&console, " B=0x"); uart_put_hex(&console, g_gic_snap[ci][1][k]);
                uart_puts(&console, "\r\n");
            }
        }
        // The headline comparison: the A72 at moment A against the A53 at moment A.
        // Both have just finished the same init sequence.
        uart_puts(&console, "[GICSNAP] core4(A) vs core1(A):");
        for (u32 k = 0; k < GICV3_SNAP_SLOTS; k++) {
            if (g_gic_snap[4][0][k] != g_gic_snap[1][0][k]) {
                uart_puts(&console, " ");
                uart_puts(&console, slot_name[k]);
                uart_puts(&console, "(4=0x"); uart_put_hex(&console, g_gic_snap[4][0][k]);
                uart_puts(&console, " 1=0x"); uart_put_hex(&console, g_gic_snap[1][0][k]);
                uart_puts(&console, ")");
            }
        }
        uart_puts(&console, "\r\n");
    }

    // RK3399 TRM: the A72 interrupt path to the GIC is clocked through CRU
    // GATES / ADB400. CRU_CLKGATE_CON12 (CRU + 0x0330) carries the two ADB400
    // clock-disable bits for the big cluster:
    //   bit 3  aclk_core_adb400_gic_2_core_b_en   GIC -> A72
    //   bit 4  aclk_core_adb400_core_b_2_gic_en   A72 -> GIC
    // and their little-cluster counterparts at bits 10 and 11, plus bit 12
    // aclk_gic_src_en. Per the TRM, 1 = disable. These are the clocks that would
    // gate the Redistributor -> CPU interface stream inside the big cluster, and
    // no diagnostic has read them per-cluster before - the April probe read
    // CLKGATE_CON33 but never decoded which bits it was looking at.
    {
        volatile u32 *cru_gates12 = (volatile u32 *)0xFF760330UL;
        volatile u32 *cru_gates33 = (volatile u32 *)0xFF760384UL;
        u32 g12 = *cru_gates12;
        u32 g33 = *cru_gates33;
                // ---- WHERE DOES THE INTERRUPT DIE? ----
                // The SGI is fired through ICC_SGI1R_EL1, the one path measured
                // to deliver. Reading HPPIR1 happens on the target while its
                // IRQs are still masked (phase B, before daifclr), so the read
                // cannot lose a race against the exception being taken; whether
                // the vector actually ran is then answered separately after
                // daifclr (phase C). Cores 1-3 are the control.
                {
                    extern volatile u64 g_icr_boundary[6][18];
                    extern void gicv3_boundary_probe_send(void);
                    u32 armed = 0;
                    for (u32 t = 0; t < 3000000u && armed < 5u; t++) {
                        armed = 0;
                        for (u32 ci = 1; ci < 6u; ci++) {
                            asm volatile("dc ivac, %0" :: "r"(&g_icr_boundary[ci][0]) : "memory");
                            asm volatile("dsb sy" ::: "memory");
                            if (g_icr_boundary[ci][0] >= 4ULL) armed++;
                        }
                        asm volatile("yield");
                    }
                    uart_puts(&console, "\r\n[BND] armed=");
                    uart_put_hex(&console, armed);
                    gicv3_boundary_probe_send();

                    u32 done = 0;
                    for (u32 t = 0; t < 6000000u && done < 5u; t++) {
                        done = 0;
                        for (u32 ci = 1; ci < 6u; ci++) {
                            asm volatile("dc ivac, %0" :: "r"(&g_icr_boundary[ci][0]) : "memory");
                            asm volatile("dsb sy" ::: "memory");
                            u64 st = g_icr_boundary[ci][0];
                            if (st == 4ULL || st == 8ULL || st == 10ULL) done++;
                        }
                        asm volatile("yield");
                    }
                    uart_puts(&console, " done=");
                    uart_put_hex(&console, done);
                    uart_puts(&console, "\r\n");

                    for (u32 ci = 1; ci < 6u; ci++) {
                        for (u32 k = 0; k < 18u; k++)
                            asm volatile("dc ivac, %0" :: "r"(&g_icr_boundary[ci][k]) : "memory");
                        asm volatile("dsb sy" ::: "memory");
                        volatile u64 *b = g_icr_boundary[ci];
                        uart_puts(&console, "[BND] core");
                        uart_put_hex(&console, ci);
                        uart_puts(&console, " step=");
                        uart_put_hex(&console, b[0]);
                        if (b[0] != 4ULL) {
                            uart_puts(&console, (b[0] == 8ULL) ? " (go timeout)" : " (phase B incomplete)");
                            uart_puts(&console, "\r\n");
                            continue;
                        }
                        uart_puts(&console, " gicr_ctlr=0x");  uart_put_hex(&console, b[1]);
                        uart_puts(&console, " icenabler=0x"); uart_put_hex(&console, b[2]);
                        uart_puts(&console, " waker=0x");     uart_put_hex(&console, b[3]);
                        uart_puts(&console, " icc_ctlr=0x");  uart_put_hex(&console, b[13]);
                        uart_puts(&console, " icc_igrpen1_pre=0x"); uart_put_hex(&console, b[14]);
                        uart_puts(&console, " post=0x"); uart_put_hex(&console, b[15]);
                        uart_puts(&console, " daif=0x");      uart_put_hex(&console, b[16]);
                        uart_puts(&console, "\r\n      sgi0: hppir_idle=0x");
                        uart_put_hex(&console, b[4] & 0x3FFULL);
                        uart_puts(&console, " -> hppir_now=0x");
                        uart_put_hex(&console, b[9] & 0x3FFULL);
                        uart_puts(&console, " | B1 pending->HPPIR: ");
                        if ((b[9] & 0x3FFULL) != 0x3FFULL) {
                            uart_puts(&console, "PASS intid=0x");
                            uart_put_hex(&console, b[9] & 0x3FFULL);
                        } else {
                            uart_puts(&console, "FAIL");
                        }
                        uart_puts(&console, " | B3 vector entries=");
                        uart_put_hex(&console, b[11]);
                        uart_puts(&console, " magic=0x");
                        uart_put_hex(&console, b[12]);
                        uart_puts(&console, "\r\n");
                    }
                    /* Dense sweep of the whole redistributor region.
                     *
                     * Reading six frames at a 0x20000 stride produced GICR_TYPER
                     * values that cannot all be real: IDbits of 1 for five frames and
                     * 17 for the sixth, CAP alternating 0/1, SGI alternating, while
                     * GICD_TYPER.IDbits is 8 and no frame agrees with it. Either the
                     * stride is wrong, or we are not reading TYPER at all.
                     *
                     * The discriminator is GICR_IIDR, not TYPER. Every redistributor
                     * in an implementation reports the same implementer/revision as
                     * the distributor, and we have already measured GICD_IIDR as
                     * 0x0041143B. An offset where IIDR reads back that value IS a
                     * redistributor RD_base frame, whatever else lives nearby.
                     *
                     * Read-only, on core 0, before interrupts are unmasked, so it
                     * cannot perturb the delivery measurement it exists to explain. */
                    {
                        u64 gicd_typer = *(volatile u32 *)0xFEE00004ULL;
                        u32 gicd_iidr  = *(volatile u32 *)0xFEE00008ULL;
                        u32 idbits = gicd_typer & 0x1Fu;
                        u32 probes = 0, iidr_hits = 0, typer_hits = 0, printed = 0;
                        uart_puts(&console, "\r\n[RSWEEP] GICD_TYPER=0x");
                        uart_put_hex(&console, gicd_typer);
                        uart_puts(&console, " IDbits=");
                        uart_put_hex(&console, idbits);
                        uart_puts(&console, " GICD_IIDR=0x");
                        uart_put_hex(&console, gicd_iidr);
                        uart_puts(&console, "\r\n");
                        /* 0x1000 granularity over 768 KiB: if the real frames are
                         * 64 KiB (RD_base only) or 128 KiB (RD_base + SGI), they are
                         * 4 KiB aligned and cannot hide between samples. */
                        for (u32 off = 0; off < 0xC0000u; off += 0x1000u) {
                            uintptr_t base = 0xFEF00000ULL + (uintptr_t)off;
                            u32 iidr = *(volatile u32 *)(base + 0x04);
                            u64 t    = *(volatile u64 *)(base + 0x08);
                            u32 lo   = (u32)t;
                            u64 aff  = t >> 32;
                            probes++;
                            u32 idb = lo & 0x1Fu;
                            u32 cap = (lo >> 10) & 1u;
                            u32 sgi = (lo >> 8) & 1u;
                            u32 iidrok = (iidr == gicd_iidr) ? 1u : 0u;
                            u32 look = iidrok | cap | (aff != 0u ? 1u : 0u);
                            if (look == 0u) continue;       /* silent: not a frame */
                            if (iidrok) iidr_hits++;
                            if (cap && sgi) typer_hits++;
                            if (printed >= 48u) continue;   /* cap the log, keep counting */
                            printed++;
                            uart_puts(&console, "  +0x");
                            uart_put_hex(&console, off);
                            uart_puts(&console, " iidr=0x");
                            uart_put_hex(&console, iidr);
                            uart_puts(&console, iidrok ? "*" : " ");
                            uart_puts(&console, " typer=0x");
                            uart_put_hex(&console, t);
                            uart_puts(&console, " idb=");
                            uart_put_hex(&console, idb);
                            uart_puts(&console, " cap=");
                            uart_put_hex(&console, cap);
                            uart_puts(&console, " sgi=");
                            uart_put_hex(&console, sgi);
                            uart_puts(&console, " aff=0x");
                            uart_put_hex(&console, aff);
                            uart_puts(&console, " (a1=");
                            uart_put_hex(&console, (u32)((aff >> 8) & 0xFFULL));
                            uart_puts(&console, " a0=");
                            uart_put_hex(&console, (u32)(aff & 0xFFULL));
                            uart_puts(&console, ")\r\n");
                        }
                        uart_puts(&console, "[RSWEEP] probed=");
                        uart_put_hex(&console, probes);
                        uart_puts(&console, " IIDR-matches=");
                        uart_put_hex(&console, iidr_hits);
                        uart_puts(&console, " CAP&SGI=");
                        uart_put_hex(&console, typer_hits);
                        uart_puts(&console, "\r\n");
                    }
                    /* MPIDR of each PE, for a like-for-like comparison against the
                     * Affinity_Value above. */
                    {
                        extern volatile u64 g_smp_pe_diag[6][8];
                        uart_puts(&console, "[RTYPER] MPIDR per PE:\r\n");
                        for (u32 f = 0; f < 6u; f++) {
                            asm volatile("dc ivac, %0" :: "r"(&g_smp_pe_diag[f][0]) : "memory");
                            asm volatile("dsb sy" ::: "memory");
                            uart_puts(&console, "  pe");
                            uart_put_hex(&console, f);
                            uart_puts(&console, " mpidr=0x");
                            uart_put_hex(&console, g_smp_pe_diag[f][3]);
                            uart_puts(&console, "\r\n");
                        }
                    }

                    /* Where did each SGI actually land? The pre-send snapshot was
                     * taken on every target while its IRQs were masked, so any
                     * delta below is attributable to the probe. A delta on a core
                     * the SGI was not addressed to means the distributor routed it
                     * somewhere wrong rather than dropping it. */
                    extern volatile u64 g_irq_entry_count[6];
                    extern volatile u64 g_icr_entry_pre[6];
                    uart_puts(&console, "\r\n[BND] handler-entry delta per core (SGI0 sent to 1,2,3,4,5):\r\n");
                    for (u32 ci = 0; ci < 6u; ci++) {
                        asm volatile("dc ivac, %0" :: "r"(&g_irq_entry_count[ci]) : "memory");
                        asm volatile("dc ivac, %0" :: "r"(&g_icr_entry_pre[ci]) : "memory");
                        asm volatile("dsb sy" ::: "memory");
                        uart_puts(&console, "   core");
                        uart_put_hex(&console, ci);
                        uart_puts(&console, " delta=");
                        uart_put_hex(&console, g_irq_entry_count[ci] - g_icr_entry_pre[ci]);
                        uart_puts(&console, "\r\n");
                    }
                }

uart_puts(&console, " [GICCLK] CON12=0x"); uart_put_hex(&console, g12);
        uart_puts(&console, " | A72 b2g="); uart_put_hex(&console, (g12 >> 4) & 1u);
        uart_puts(&console, " g2b=");        uart_put_hex(&console, (g12 >> 3) & 1u);
        uart_puts(&console, " | A53 b2g="); uart_put_hex(&console, (g12 >> 11) & 1u);
        uart_puts(&console, " g2b=");        uart_put_hex(&console, (g12 >> 10) & 1u);
        uart_puts(&console, " | gic_src=");   uart_put_hex(&console, (g12 >> 12) & 1u);
        uart_puts(&console, " CON33=0x");      uart_put_hex(&console, g33);
        uart_puts(&console, " | g33 L2G=");    uart_put_hex(&console, (g33 >> 2) & 1u);
        uart_puts(&console, " B2G=");         uart_put_hex(&console, (g33 >> 3) & 1u);
        uart_puts(&console, " G2L=");         uart_put_hex(&console, (g33 >> 4) & 1u);
        uart_puts(&console, " G2B=");         uart_put_hex(&console, (g33 >> 5) & 1u);
        uart_puts(&console, "\r\n");
    }
            }
            uart_puts(&console, "\r\n");
        }
    }

    // Secure-world GICR diagnostics (requires BL31 SiP patch):
    // GET:  x0=status, x1=waker
    // WAKE: x0=flags,  x1=before, x2=after
    // If patch is absent, status typically returns SMC_UNK (0xFFFFFFFF).
    {
        sip_gicr_get_t g4 = sip_gicr_waker_get(4);
        sip_gicr_get_t g5 = sip_gicr_waker_get(5);
        sip_gicr_wake_try_t w4 = sip_gicr_wake_try(4);
        sip_gicr_wake_try_t w5 = sip_gicr_wake_try(5);

        uart_puts(&console, "[GICR_SIP] get core4 st=0x");
        uart_put_hex(&console, g4.status);
        uart_puts(&console, " waker=0x");
        uart_put_hex(&console, g4.waker);
        uart_puts(&console, " | core5 st=0x");
        uart_put_hex(&console, g5.status);
        uart_puts(&console, " waker=0x");
        uart_put_hex(&console, g5.waker);
        uart_puts(&console, "\r\n");

        uart_puts(&console, "[GICR_SIP] wake core4 flags=0x");
        uart_put_hex(&console, w4.status_flags);
        uart_puts(&console, " before=0x");
        uart_put_hex(&console, w4.before);
        uart_puts(&console, " after=0x");
        uart_put_hex(&console, w4.after);
        uart_puts(&console, " | core5 flags=0x");
        uart_put_hex(&console, w5.status_flags);
        uart_puts(&console, " before=0x");
        uart_put_hex(&console, w5.before);
        uart_puts(&console, " after=0x");
        uart_put_hex(&console, w5.after);
        uart_puts(&console, "\r\n");
    }

    LOG_OK("WQ: Work queue initialized");
    dbg_putc_raw('>');
    dbg_putc_raw('W');
    dbg_putc_raw('<');
    dbg_putc_raw('\r');
    dbg_putc_raw('\n');

    uart_puts(&console, "[DBG] after WQ\r\n");
    smp_dump_diagnostics(&console);

    // Phase 3.1: Mali-T860 GPU power-on (bare-metal, no Linux drivers)
    if (mali_power_on() == OK) {
        LOG_OK("Mali-T860: GPU powered on (bare-metal Panfrost MMIO)");
        mali_dump_status();
        // Phase 3.2: Initialize Job Manager
        if (mali_jm_init() == OK) {
            LOG_OK("Mali-T860: Job Manager ready");
        }
        // Phase 3.3: Initialize MMU with identity mapping (CCI-500 zero-copy)
        if (mali_mmu_init() == OK) {
            LOG_OK("Mali-T860: MMU identity-mapped (4GB, CCI-500 ACE-Lite)");
            // Phase 3.4: Run NULL job smoke test to verify JM/MMU pipeline
            if (mali_compute_smoke_test() == OK) {
                LOG_OK("Mali-T860: Smoke test PASSED (NULL + WRITE_VALUE + CACHE_FLUSH + COMPUTE_PROBE)");
                // Phase 3.5: 4×4 matrix multiply via GPU compute shader
                if (mali_compute_matmul_4x4() == OK) {
                    LOG_OK("Mali-T860: MATMUL 4x4 PASSED (I*I=I)");
                } else {
                    LOG_WARN("Mali-T860: MATMUL 4x4 FAILED");
                }
            } else {
                LOG_WARN("Mali-T860: Smoke test FAILED");
            }
        } else {
            LOG_WARN("Mali-T860: MMU init failed");
        }
    } else {
        LOG_WARN("Mali-T860: power-on failed (PD_GPU not enabled by BL31?)");
    }

    // 3. Initialize GMAC (Networking)
    if (gmac_init() == OK) {
        LOG_OK("GMAC: PHY Reset & MAC Configured");

        // Send L2 announcement frame (broadcast, EtherType 0x88EE)
        static u8 frame[32];
        const u8* src = gmac_get_mac();
        for (u32 i = 0; i < 6; i++) frame[i] = 0xFF;          // dst: broadcast
        for (u32 i = 0; i < 6; i++) frame[6 + i] = src[i];    // src: our MAC
        frame[12] = 0x88; frame[13] = 0xEE;                    // EtherType H-Exo
        frame[14]='H'; frame[15]='-'; frame[16]='E'; frame[17]='x'; frame[18]='o';
        frame[19]='-'; frame[20]='v'; frame[21]='0'; frame[22]='.'; frame[23]='9';
        for (u32 i = 24; i < 32; i++) frame[i] = 0;           // pad to min 32 bytes

        if (gmac_send_raw(frame, 32) == OK) {
            LOG_OK("GMAC: L2 beacon sent (0x88EE broadcast)");
        } else {
            LOG_WARN("GMAC: L2 beacon TX failed");
        }
        // Enable GMAC DMA RX interrupt via GICv3
        gicv3_route_irq(GMAC_GIC_INTID, 0x0);  // route to core 0
        gicv3_set_priority(GMAC_GIC_INTID, 0xA0);
        gicv3_enable_irq(GMAC_GIC_INTID);
        gmac_irq_enable();
        LOG_OK("GMAC: RX interrupt enabled (SPI 24)");
    } else {
        LOG_ERR("GMAC: Initialization Failed");
    }

    print_banner();

    // Phase 4.1: Initialize TSADC thermal sensor (CPU + GPU)
    if (tsadc_init() == OK) {
        LOG_OK("TSADC: Thermal sensor ready (CPU + GPU channels)");
        tsadc_dump();
        // Phase 4.2: Initialize thermal guard
        if (thermal_guard_init(&g_thermal_guard) == OK) {
            LOG_OK("Thermal Guard: workload throttling armed");
            thermal_guard_update(&g_thermal_guard);
            thermal_guard_dump(&g_thermal_guard);
        }
    } else {
        LOG_WARN("TSADC: Initialization failed");
    }

    // Low-level eMMC operability probe: PMIC rails + host/PHY read-only snapshot.
    emmc_low_level_diag_t emmc_diag;
    i32 emmc_ret = cru_emmc_low_level_probe(&emmc_diag);
    uart_puts(&console, "\r\n[EMMC_DIAG] i2c_ret=0x");
    uart_put_hex(&console, (u64)emmc_diag.i2c_ret);
    uart_puts(&console, " rk808_id1=0x");
    uart_put_hex(&console, emmc_diag.rk808_chip_id);
    uart_puts(&console, " rk808_buck2=0x");
    uart_put_hex(&console, emmc_diag.rk808_buck2_on);
    uart_puts(&console, " rk808_23=0x");
    uart_put_hex(&console, emmc_diag.rk808_reg23);
    uart_puts(&console, " rk808_24=0x");
    uart_put_hex(&console, emmc_diag.rk808_reg24);
    uart_puts(&console, " sw1=0x");
    uart_put_hex(&console, emmc_diag.sw1_en);
    uart_puts(&console, " sw2=0x");
    uart_put_hex(&console, emmc_diag.sw2_en);
    uart_puts(&console, " ldo6=0x");
    uart_put_hex(&console, emmc_diag.ldo6_vsel);
    uart_puts(&console, " ldo9=0x");
    uart_put_hex(&console, emmc_diag.ldo9_vsel);
    uart_puts(&console, " ldo_en2=0x");
    uart_put_hex(&console, emmc_diag.ldo_en2);
    uart_puts(&console, "\r\n");
    uart_puts(&console, "[EMMC_DIAG] mmc ctrl=0x");
    uart_put_hex(&console, emmc_diag.mmc_ctrl);
    uart_puts(&console, " pwren=0x");
    uart_put_hex(&console, emmc_diag.mmc_pwren);
    uart_puts(&console, " clkena=0x");
    uart_put_hex(&console, emmc_diag.mmc_clkena);
    uart_puts(&console, " cdetect=0x");
    uart_put_hex(&console, emmc_diag.mmc_cdetect);
    uart_puts(&console, " status=0x");
    uart_put_hex(&console, emmc_diag.mmc_status);
    uart_puts(&console, " rint=0x");
    uart_put_hex(&console, emmc_diag.mmc_rintsts);
    uart_puts(&console, "\r\n[EMMC_DIAG] grf_soc_con22=0x");
    uart_put_hex(&console, emmc_diag.grf_soc_con22);
    uart_puts(&console, " phy_con0=0x");
    uart_put_hex(&console, emmc_diag.emmc_phy_con0);
    uart_puts(&console, " phy_status=0x");
    uart_put_hex(&console, emmc_diag.emmc_phy_status);
    uart_puts(&console, "\r\n");
    if (emmc_ret == 0) {
        LOG_OK("eMMC: low-level probe captured");
    } else {
        uart_puts(&console, "[INFO] eMMC: low-level probe PMIC read unavailable, continuing boot\r\n");
    }

    // Attempt eMMC recovery if PHY is not ready, but do not block boot on failure.
    // The NanoPi M4 eMMC module may be physically dead (removable module format).
    if (emmc_ret == 0 &&
        (emmc_diag.sw1_en == 0 || emmc_diag.sw2_en == 0 ||
         ((emmc_diag.emmc_phy_status & 1u) == 0))) {
        i32 emmc_fix = cru_emmc_recover_power_and_phy();
        uart_puts(&console, "[EMMC_RECOVER] ret=0x");
        uart_put_hex(&console, (u64)emmc_fix);
        uart_puts(&console, "\r\n");

        emmc_low_level_diag_t emmc_post;
        i32 emmc_post_ret = cru_emmc_low_level_probe(&emmc_post);
        uart_puts(&console, "[EMMC_DIAG_POST] i2c_ret=0x");
        uart_put_hex(&console, (u64)emmc_post.i2c_ret);
        uart_puts(&console, " rk808_id1=0x");
        uart_put_hex(&console, emmc_post.rk808_chip_id);
        uart_puts(&console, " rk808_buck2=0x");
        uart_put_hex(&console, emmc_post.rk808_buck2_on);
        uart_puts(&console, " rk808_23=0x");
        uart_put_hex(&console, emmc_post.rk808_reg23);
        uart_puts(&console, " sw1=0x");
        uart_put_hex(&console, emmc_post.sw1_en);
        uart_puts(&console, " sw2=0x");
        uart_put_hex(&console, emmc_post.sw2_en);
        uart_puts(&console, " ldo6=0x");
        uart_put_hex(&console, emmc_post.ldo6_vsel);
        uart_puts(&console, " ldo9=0x");
        uart_put_hex(&console, emmc_post.ldo9_vsel);
        uart_puts(&console, " ldo_en2=0x");
        uart_put_hex(&console, emmc_post.ldo_en2);
        uart_puts(&console, " mmc_status=0x");
        uart_put_hex(&console, emmc_post.mmc_status);
        uart_puts(&console, " rint=0x");
        uart_put_hex(&console, emmc_post.mmc_rintsts);
        uart_puts(&console, " phy_status=0x");
        uart_put_hex(&console, emmc_post.emmc_phy_status);
        uart_puts(&console, "\r\n");

        if (emmc_fix == 0 && emmc_post_ret == 0 && ((emmc_post.emmc_phy_status & 1u) != 0)) {
            LOG_OK("eMMC: recovery sequence applied (PHY ready)");
        } else {
            uart_puts(&console, "[INFO] eMMC: module not responding (likely hardware dead), continuing boot\r\n");
        }
    }

    // Initialize PMU for Phase 0 baseline measurements
    u32 pmu_counters = pmu_init();
    if (pmu_counters > 0) {
        LOG_OK("PMU: Phase 0 baseline measurement ready");
        uart_puts(&console, "[PMU] counters=");
        uart_put_hex(&console, pmu_counters);
        uart_puts(&console, "\r\n");
        pmu_enable();
    } else {
        LOG_WARN("PMU: Initialization failed (running without PMU)");
    }

    // Initialize Neural Arbitrator
    LOG_INFO("Initializing Neural Arbitrator...");
    result_t res = neuro_sync_init(&neural_arbitrator);
    if (res == OK) {
        LOG_OK("Neuro-Sync: TinyML Engine Ready");
        LOG_OK("Model: 6->8->4 Feedforward Network");
        LOG_OK("Arithmetic: Fixed-Point Q16.16");
        
        u32 expected_crc = get_expected_weights_crc();
        u32 actual_crc   = compute_weights_crc32(neural_arbitrator.weights);
        if (actual_crc == expected_crc) {
            LOG_OK("Neural weights integrity verified");
        } else {
            // Print computed CRC so it can be hard-coded in
            // neuro/weight_validation.c::get_expected_weights_crc().
            uart_puts(&console, "[WARN] Neural weights CRC mismatch: actual=0x");
            uart_put_hex(&console, actual_crc);
            uart_puts(&console, " expected=0x");
            uart_put_hex(&console, expected_crc);
            uart_puts(&console, " (update get_expected_weights_crc())\r\n");
        }

        // Initialize Adaptive Scheduler (EMA jitter feedback loop)
        adaptive_scheduler_init(&adaptive_sched, &neural_arbitrator, NULL);

        // Initialize parallel inference (uses all 6 cores: 4 for hidden, 1 for output)
        neuro_parallel_init(neural_arbitrator.weights);
        LOG_OK("Neuro-Parallel: 6-core inference ready (4x hidden + 1x output)");
        
        // Phase 2: Initialize Pipe-it pipeline (3-stage with GICv3 SGI)
        // Don't start yet — core 4/5 must be in WFE workqueue mode for baseline.
        // Pipeline activates after baseline completes (see pipeit_start below).
        if (pipeit_init(&g_pipeit, &g_pipe_buffer, neural_arbitrator.weights) == OK) {
            LOG_OK("Pipe-it: 3-stage pipeline initialized (SGI-driven)");
        } else {
            LOG_WARN("Pipe-it: initialization failed");
        }
        
        // Phase 5.1: L2 binary protocol
        hexo_l2_init();
        // Phase 5.2: PTP-style clock sync
        hexo_ptp_init();
        // Phase 5.3: Task offload with retransmission
        hexo_offload_init();
        // Phase 5.4: Gossip federated weight averaging
        // CRITICAL: gossip needs writable weights (not ROM)
        gossip_init(&g_gossip, neuro_sync_get_writable_weights());
        // Phase 6.1: Multi-node peer table
        peer_table_init();

        // Phase 4.3: Initialize WCET regions
        wcet_init(&g_wcet_inference, "inference");
        wcet_init(&g_wcet_pipeit, "pipeit_frame");

        // Warmup inference - measure latency (use nominal values, telemetry_init not yet called)
        telemetry_t warmup_tel = { 50, 100, 20, 50, 0, 1 };
        inference_result_t warmup_result;
        u64 inf_start = read_cntpct();
        wcet_begin(&g_wcet_inference);
        adaptive_inference(&adaptive_sched, &warmup_tel, &warmup_result);
        wcet_end(&g_wcet_inference);
        u64 inf_us = (read_cntpct() - inf_start) / 24;
        LOG_OK("Adaptive Scheduler: EMA feedback loop ready");
        uart_puts(&console, "[PERF] inference_us=0x");
        uart_put_hex(&console, inf_us);
        uart_puts(&console, "\r\n");

        // Print inference result
        static const char* const hint_str[] = { "STAY", "MIGRATE_PERF", "MIGRATE_POWER" };
        static const char* const pwr_str[]  = { "SLEEP", "IDLE", "ACTIVE", "TURBO" };
        u8 hint = warmup_result.migration_hint & 0x3;
        u8 pwr  = warmup_result.power_state  & 0x3;
        uart_puts(&console, "[SCHED] hint=");
        uart_puts(&console, hint_str[hint]);
        uart_puts(&console, " power=");
        uart_puts(&console, pwr_str[pwr]);
        uart_puts(&console, " trust=0x");
        uart_put_hex(&console, warmup_result.trust_score);
        uart_puts(&console, " stability=0x");
        uart_put_hex(&console, adaptive_sched.stability_score);
        uart_puts(&console, "\r\n");

        // Phase 0.4: full PMU baseline (10000 inferences). PMCCNTR_EL0 +
        // event counters now tick at NS EL2 thanks to PMCCFILTR/PMEVTYPER
        // NSH=1 fix in hal/pmu.c. CNTPCT kept as secondary wall-clock signal.
        // Counter assignments (see hal/pmu.c default_events[]):
        //   PMCCNTR_EL0 -> CPU cycles
        //   1 -> INST_RETIRED
        //   2 -> L1D_CACHE_REFILL  (L1D misses)
        //   3 -> L2D_CACHE_REFILL  (L2D misses)
        //   4 -> BUS_CYCLES
        //   5 -> BR_MIS_PRED       (branch mispredicts)
        uart_puts(&console, "\r\n[BASELINE] Starting full PMU timing loop (10000 inferences)...\r\n");
        pmu_snapshot_t b_start, b_end, b_delta;
        u64 t_min = ~0ULL;
        u64 t_max = 0;
        u64 t_sum = 0;
        u64 t_sum_sq = 0;
        u64 cyc_min = ~0ULL, cyc_max = 0, cyc_sum = 0;
        u64 inst_sum = 0, l1m_sum = 0, l2m_sum = 0, bus_sum = 0, br_sum = 0;

        // Phase 1.4: warmup pass — populate L1i/L1d on the boot CPU (A53#0)
        // before the measured loop. Eliminates cold-cache outlier on iter 0.
        // Discarded results, same input frame as the measured loop.
        {
            inference_result_t wr;
            for (u32 wu = 0; wu < 32u; wu++) {
                adaptive_inference(&adaptive_sched, &warmup_tel, &wr);
            }
            asm volatile("dsb ish; isb" ::: "memory");
        }

        u64 baseline_t0 = read_cntpct();
        for (u32 it = 0; it < PMU_BASELINE_COUNT; it++) {
            // Phase 1.3 (ADL-016): mask IRQ around the per-iter measurement
            // window. Boot CPU (A53#0) services timer/SGI which would otherwise
            // inflate cyc_max outliers. Inference is local (no wq_dispatch),
            // so safe to mask. Restored before the next iteration's overhead.
            u64 daif_save;
            pmu_meas_critical_enter(&daif_save);
            u64 ts0 = read_cntpct();
            pmu_take_snapshot(&b_start);
            inference_result_t r;
            adaptive_inference(&adaptive_sched, &warmup_tel, &r);
            pmu_take_snapshot(&b_end);
            u64 dt = read_cntpct() - ts0;
            pmu_meas_critical_exit(daif_save);
            pmu_calc_delta(&b_start, &b_end, &b_delta);
            if (dt < t_min) t_min = dt;
            if (dt > t_max) t_max = dt;
            t_sum    += dt;
            t_sum_sq += dt * dt;
            if (b_delta.cycle_count < cyc_min) cyc_min = b_delta.cycle_count;
            if (b_delta.cycle_count > cyc_max) cyc_max = b_delta.cycle_count;
            cyc_sum  += b_delta.cycle_count;
            inst_sum += b_delta.instructions;
            l1m_sum  += b_delta.l1d_cache_refill;
            l2m_sum  += b_delta.l2d_cache_refill;
            bus_sum  += b_delta.bus_cycles;
            br_sum   += b_delta.branch_mispred;
        }
        u64 baseline_total_ticks = read_cntpct() - baseline_t0;
        u64 baseline_us = baseline_total_ticks / 24;
        u64 avg_ticks  = t_sum / PMU_BASELINE_COUNT;
        u64 avg_ns     = (t_sum * 125) / (PMU_BASELINE_COUNT * 3);  // 1 tick = 41.666ns
        u64 min_ns     = (t_min * 125) / 3;
        u64 max_ns     = (t_max * 125) / 3;
        u64 mean_sq = avg_ticks * avg_ticks;
        u64 e_x_sq  = t_sum_sq / PMU_BASELINE_COUNT;
        u64 variance_ticks = (e_x_sq > mean_sq) ? (e_x_sq - mean_sq) : 0;
        u64 avg_cycles = cyc_sum / PMU_BASELINE_COUNT;
        u64 avg_inst   = inst_sum / PMU_BASELINE_COUNT;
        u64 avg_l1m    = l1m_sum / PMU_BASELINE_COUNT;
        u64 avg_l2m    = l2m_sum / PMU_BASELINE_COUNT;
        u64 avg_bus    = bus_sum / PMU_BASELINE_COUNT;
        u64 avg_br     = br_sum / PMU_BASELINE_COUNT;
        // IPC * 1000 to keep integer math.
        u64 ipc_x1000  = (avg_cycles > 0) ? ((avg_inst * 1000) / avg_cycles) : 0;
        // CPU freq derived from cycles/wall-time (hint at actual core clock).
        // freq_hz = (cycles per inference) * (inferences per second)
        //        = avg_cycles * 1e6 / avg_ns  -> express in MHz: avg_cycles * 1000 / avg_ns
        u64 cpu_mhz    = (avg_ns > 0) ? ((avg_cycles * 1000) / avg_ns) : 0;
        // Phase 0.2: authoritative freq via PMCCNTR/CNTPCT over 1 ms window.
        u32 cpu_mhz_actual = pmu_measure_cpu_freq_mhz(1000u);
        u64 ddr_mbps   = baseline_total_ticks ? (bus_sum * 64 * 24) / baseline_total_ticks : 0;
        
        // Persist for legacy printers and PMU baseline status
        pmu_inference_count = PMU_BASELINE_COUNT;
        pmu_total_cycles    = cyc_sum;
        pmu_total_l1_misses = l1m_sum;
        pmu_total_l2_misses = l2m_sum;
        
        uart_puts(&console, "[BASELINE] COMPLETE\r\n");
        uart_puts(&console, "[BASELINE] total_inferences=0x"); uart_put_hex(&console, PMU_BASELINE_COUNT);
        uart_puts(&console, "\r\n[BASELINE] elapsed_us=0x");   uart_put_hex(&console, baseline_us);
        uart_puts(&console, "\r\n[BASELINE] avg_cycles=0x");   uart_put_hex(&console, avg_cycles);
        uart_puts(&console, "\r\n[BASELINE] min_cycles=0x");   uart_put_hex(&console, cyc_min);
        uart_puts(&console, "\r\n[BASELINE] max_cycles=0x");   uart_put_hex(&console, cyc_max);
        uart_puts(&console, "\r\n[BASELINE] avg_inst=0x");     uart_put_hex(&console, avg_inst);
        uart_puts(&console, "\r\n[BASELINE] avg_l1d_miss=0x"); uart_put_hex(&console, avg_l1m);
        uart_puts(&console, "\r\n[BASELINE] avg_l2d_miss=0x"); uart_put_hex(&console, avg_l2m);
        uart_puts(&console, "\r\n[BASELINE] avg_bus_cyc=0x");  uart_put_hex(&console, avg_bus);
        uart_puts(&console, "\r\n[BASELINE] avg_br_mispr=0x"); uart_put_hex(&console, avg_br);
        uart_puts(&console, "\r\n[BASELINE] ddr_mbps=0x");     uart_put_hex(&console, ddr_mbps);
        uart_puts(&console, "\r\n[BASELINE] ipc_x1000=0x");    uart_put_hex(&console, ipc_x1000);
        uart_puts(&console, "\r\n[BASELINE] cpu_mhz=0x");      uart_put_hex(&console, cpu_mhz);
        uart_puts(&console, "\r\n[BASELINE] cpu_mhz_actual=0x"); uart_put_hex(&console, (u64)cpu_mhz_actual);
        uart_puts(&console, "\r\n[BASELINE] avg_ticks=0x");    uart_put_hex(&console, avg_ticks);
        uart_puts(&console, "\r\n[BASELINE] min_ticks=0x");    uart_put_hex(&console, t_min);
        uart_puts(&console, "\r\n[BASELINE] max_ticks=0x");    uart_put_hex(&console, t_max);
        uart_puts(&console, "\r\n[BASELINE] var_ticks=0x");    uart_put_hex(&console, variance_ticks);
        uart_puts(&console, "\r\n[BASELINE] avg_ns=0x");       uart_put_hex(&console, avg_ns);
        uart_puts(&console, "\r\n[BASELINE] min_ns=0x");       uart_put_hex(&console, min_ns);
        uart_puts(&console, "\r\n[BASELINE] max_ns=0x");       uart_put_hex(&console, max_ns);
        uart_puts(&console, "\r\n[BASELINE] === BASELINE_JSON_BEGIN ===\r\n");
        uart_puts(&console, "{\"version\":\"v2\",\"timer\":\"pmccntr+cntpct\",\"n\":0x");
        uart_put_hex(&console, PMU_BASELINE_COUNT);
        uart_puts(&console, ",\"avg_cycles\":0x");   uart_put_hex(&console, avg_cycles);
        uart_puts(&console, ",\"min_cycles\":0x");   uart_put_hex(&console, cyc_min);
        uart_puts(&console, ",\"max_cycles\":0x");   uart_put_hex(&console, cyc_max);
        uart_puts(&console, ",\"avg_inst\":0x");     uart_put_hex(&console, avg_inst);
        uart_puts(&console, ",\"avg_l1d_miss\":0x"); uart_put_hex(&console, avg_l1m);
        uart_puts(&console, ",\"avg_l2d_miss\":0x"); uart_put_hex(&console, avg_l2m);
        uart_puts(&console, ",\"avg_bus_cyc\":0x");  uart_put_hex(&console, avg_bus);
        uart_puts(&console, ",\"avg_br_mispr\":0x"); uart_put_hex(&console, avg_br);
        uart_puts(&console, ",\"ddr_mbps\":0x");     uart_put_hex(&console, ddr_mbps);
        uart_puts(&console, ",\"ipc_x1000\":0x");    uart_put_hex(&console, ipc_x1000);
        uart_puts(&console, ",\"cpu_mhz\":0x");      uart_put_hex(&console, cpu_mhz);
        uart_puts(&console, ",\"cpu_mhz_actual\":0x"); uart_put_hex(&console, (u64)cpu_mhz_actual);
        uart_puts(&console, ",\"avg_ticks\":0x");    uart_put_hex(&console, avg_ticks);
        uart_puts(&console, ",\"min_ticks\":0x");    uart_put_hex(&console, t_min);
        uart_puts(&console, ",\"max_ticks\":0x");    uart_put_hex(&console, t_max);
        uart_puts(&console, ",\"var_ticks\":0x");    uart_put_hex(&console, variance_ticks);
        uart_puts(&console, ",\"avg_ns\":0x");       uart_put_hex(&console, avg_ns);
        uart_puts(&console, "}\r\n[BASELINE] === BASELINE_JSON_END ===\r\n");
        
        // === Phase 0.5: replay identical baseline on A72 core 4 ===
        uart_puts(&console, "\r\n[BASELINE_A72] Dispatching baseline to A72 core 4 @ 1608 MHz...\r\n");
        g_a72_warmup_tel = warmup_tel;
        g_a72_baseline.done_flag = 0;
        asm volatile("dmb ish" ::: "memory");
        wq_dispatch(4, baseline_runner_a72, 0);
        
        // Wait, but NOT 30 s: a wedged A72 here would swallow the whole boot
        // capture (diagnostics that follow - SGI_TEST, the decisive delivery
        // check - would never print). 1.5 s is enough for a baseline that runs
        // in ~0.5 ms when the workqueue actually reaches the A72. If it does
        // time out we print that and continue; SGI_TEST below is the answer.
        u64 wait_t0 = read_cntpct();
        const u64 wait_timeout = 24ULL * 1000 * 1000 * 15 / 10;
        while (g_a72_baseline.done_flag == 0) {
            if (read_cntpct() - wait_t0 > wait_timeout) break;
            asm volatile("yield");
        }
        if (g_a72_baseline.done_flag == 0) {
            uart_puts(&console, "[BASELINE_A72] TIMEOUT waiting for done (A72 did not pick up the job)\r\n");
        }
        asm volatile("dmb ish" ::: "memory");

        if (g_a72_baseline.done_flag) {
            uart_puts(&console, "[BASELINE_A72] target_mhz=0x");     uart_put_hex(&console, g_a72_baseline.clk_target_mhz);
            uart_puts(&console, " applied_mhz=0x");                   uart_put_hex(&console, g_a72_baseline.clk_applied_mhz);
            uart_puts(&console, " clk_set_ret=0x");                   uart_put_hex(&console, (u64)g_a72_baseline.clk_set_ret);
            uart_puts(&console, " first_ret=0x");                     uart_put_hex(&console, (u64)g_a72_baseline.clk_first_ret);
            if (g_a72_baseline.clk_first_ret <= -100) {
                uart_puts(&console, " pmic_id1=0x");                  uart_put_hex(&console, g_a72_baseline.clk_diag_rk808_id1);
                uart_puts(&console, " pmic_i2c=0x");                  uart_put_hex(&console, g_a72_baseline.clk_diag_i2c_status);
            }
            uart_puts(&console, "\r\n[BASELINE_A72] avg_cycles=0x");  uart_put_hex(&console, g_a72_baseline.avg_cycles);
            uart_puts(&console, " avg_ns=0x");                        uart_put_hex(&console, g_a72_baseline.avg_ns);
            uart_puts(&console, " ipc=0x");                           uart_put_hex(&console, g_a72_baseline.ipc_x1000);
            uart_puts(&console, " cpu_mhz=0x");                      uart_put_hex(&console, g_a72_baseline.cpu_mhz);
            uart_puts(&console, " cpu_mhz_actual=0x");               uart_put_hex(&console, g_a72_baseline.cpu_mhz_actual);
            uart_puts(&console, " avg_asimd=0x");                    uart_put_hex(&console, g_a72_baseline.avg_asimd);
            uart_puts(&console, "\r\n[BASELINE_A72] === BASELINE_JSON_BEGIN ===\r\n");
            uart_puts(&console, "{\"version\":\"v7\",\"timer\":\"pmccntr+cntpct\",\"core\":\"A72\",\"mpidr\":0x");
            uart_put_hex(&console, g_a72_baseline.mpidr);
            uart_puts(&console, ",\"clk_target_mhz\":0x"); uart_put_hex(&console, g_a72_baseline.clk_target_mhz);
            uart_puts(&console, ",\"clk_applied_mhz\":0x");uart_put_hex(&console, g_a72_baseline.clk_applied_mhz);
            uart_puts(&console, ",\"clk_set_ret\":0x");    uart_put_hex(&console, (u64)g_a72_baseline.clk_set_ret);
            uart_puts(&console, ",\"clk_first_ret\":0x");  uart_put_hex(&console, (u64)g_a72_baseline.clk_first_ret);
            uart_puts(&console, ",\"pmic_id1\":0x");       uart_put_hex(&console, g_a72_baseline.clk_diag_rk808_id1);
            uart_puts(&console, ",\"pmic_i2c\":0x");       uart_put_hex(&console, g_a72_baseline.clk_diag_i2c_status);
            uart_puts(&console, ",\"n\":0x");               uart_put_hex(&console, PMU_BASELINE_COUNT);
            uart_puts(&console, ",\"avg_cycles\":0x");     uart_put_hex(&console, g_a72_baseline.avg_cycles);
            uart_puts(&console, ",\"min_cycles\":0x");     uart_put_hex(&console, g_a72_baseline.min_cycles);
            uart_puts(&console, ",\"max_cycles\":0x");     uart_put_hex(&console, g_a72_baseline.max_cycles);
            uart_puts(&console, ",\"avg_inst\":0x");       uart_put_hex(&console, g_a72_baseline.avg_inst);
            uart_puts(&console, ",\"avg_l1d_miss\":0x");   uart_put_hex(&console, g_a72_baseline.avg_l1m);
            uart_puts(&console, ",\"avg_l2d_miss\":0x");   uart_put_hex(&console, g_a72_baseline.avg_l2m);
            uart_puts(&console, ",\"avg_bus_cyc\":0x");    uart_put_hex(&console, g_a72_baseline.avg_bus);
            uart_puts(&console, ",\"avg_br_mispr\":0x");   uart_put_hex(&console, g_a72_baseline.avg_br);
            uart_puts(&console, ",\"avg_asimd\":0x");     uart_put_hex(&console, g_a72_baseline.avg_asimd);
            uart_puts(&console, ",\"ddr_mbps\":0x");       uart_put_hex(&console, g_a72_baseline.ddr_mbps);
            uart_puts(&console, ",\"ipc_x1000\":0x");      uart_put_hex(&console, g_a72_baseline.ipc_x1000);
            uart_puts(&console, ",\"cpu_mhz\":0x");        uart_put_hex(&console, g_a72_baseline.cpu_mhz);
            uart_puts(&console, ",\"cpu_mhz_actual\":0x"); uart_put_hex(&console, g_a72_baseline.cpu_mhz_actual);
            uart_puts(&console, ",\"avg_ticks\":0x");      uart_put_hex(&console, g_a72_baseline.avg_ticks);
            uart_puts(&console, ",\"min_ticks\":0x");      uart_put_hex(&console, g_a72_baseline.min_ticks);
            uart_puts(&console, ",\"max_ticks\":0x");      uart_put_hex(&console, g_a72_baseline.max_ticks);
            uart_puts(&console, ",\"var_ticks\":0x");      uart_put_hex(&console, g_a72_baseline.var_ticks);
            uart_puts(&console, ",\"avg_ns\":0x");         uart_put_hex(&console, g_a72_baseline.avg_ns);
            uart_puts(&console, "}\r\n[BASELINE_A72] === BASELINE_JSON_END ===\r\n");
        } else {
            uart_puts(&console, "[BASELINE_A72] TIMEOUT - A72 core 4 did not complete in 30s\r\n");
        }
        
        // === Phase 2: Pipeline baseline benchmark ===
        // Pipeline is not yet started (g_pipeit_active=0), so we need to
        // temporarily start it, run frames, then stop for the baseline runner.
        // Instead, we run the pipeline benchmark AFTER baseline, using
        // the already-started pipeline (pipeit_start called later).
        // For now, just report that pipeline benchmark will run at operational phase.
    } else {
        LOG_ERR("Neuro-Sync: Initialization Failed");
    }

    res = telemetry_init(&telemetry);
    if (res == OK) {
        LOG_OK("Telemetry: Generic Timer (24MHz) Active");
        LOG_OK("Telemetry: Runtime metrics Active");
    }

    // Boot time measurement
    u64 boot_us = (read_cntpct() - boot_start) / 24;
    uart_puts(&console, "[PERF] boot_time_us=0x");
    uart_put_hex(&console, boot_us);
    uart_puts(&console, "\r\n");

    uart_puts(&console, "\r\n========================================\r\n");
    uart_puts(&console, "  H-Exo Omni-Core: Operational\r\n");
    uart_puts(&console, "  Adaptive Neural Fabric Ready\r\n");
    uart_puts(&console, "========================================\r\n");
    heartbeat_init(&console);
    runtime_last_tick_cycles = 0;
    runtime_loop_jitter_percent = 0;
    runtime_last_offload_state = false;
    
    // Minimal post-baseline checks (slow tests disabled for faster iteration)
    uart_puts(&console, "[OK] Boot complete\r\n");
    
    // Unmask IRQ at EL2 BEFORE pipeline start so the SGI self-test can run
    // (sgi_counters update only happens when handle_irq_exception fires, which
    // requires PSTATE.I=0 on core 0).
    asm volatile("msr daifclr, #2" ::: "memory");
    LOG_OK("IRQ: EL2 unmasked -- interrupt-driven network active");
    
    // === SGI delivery self-test (BEFORE pipeit_start) ===
    // Markers placed at every link in the SGI chain so we can pinpoint the
    // break: sender MSR -> distributor -> redistributor pending -> PE IRQ.
    {
        extern volatile u64 sgi_counters[6][4];
        // -- distributor health --
        u32 gicd_ctlr_rb = gicv3_read_gicd_ctlr();
        u64 gicd_typer_rb = gicv3_read_gicd_typer();
        uart_puts(&console, "[SGI_TEST] gicd_ctlr=0x"); uart_put_hex(&console, gicd_ctlr_rb);
        uart_puts(&console, " gicd_typer=0x");          uart_put_hex(&console, gicd_typer_rb);
        uart_puts(&console, "\r\n");
        // -- DAIF on core 0 (must have I=0 at this point) --
        u64 daif_c0;
        asm volatile("mrs %0, daif" : "=r"(daif_c0));
        uart_puts(&console, "[SGI_TEST] core0 daif=0x"); uart_put_hex(&console, daif_c0);
        uart_puts(&console, "\r\n");
        // -- send_count BEFORE --
        u64 sc_before = g_gicv3_sgi_send_count;
        u64 c1_before = sgi_counters[1][SGI_STAGE_HIDDEN];
        u64 c4_before = sgi_counters[4][SGI_STAGE_HIDDEN];
        u32 isp1_before = gicv3_read_ispendr0(1);
        u32 isp4_before = gicv3_read_ispendr0(4);
        // -- fire SGI 1 to core 1 --
        gicv3_sgi_send(SGI_STAGE_HIDDEN, 0x001);
        // immediate ISPENDR readback (before any handler can clear)
        u32 isp1_imm = gicv3_read_ispendr0(1);
        // -- fire SGI 1 to core 4 --
        gicv3_sgi_send(SGI_STAGE_HIDDEN, 0x100);
        u32 isp4_imm = gicv3_read_ispendr0(4);
        // Secondaries idle in WFE during this pre-pipeline probe.
        // Nudge them with SEV so they exit WFE and can service pending SGI.
        asm volatile("sev" ::: "memory");
        asm volatile("sev" ::: "memory");
        // wait 10ms for handlers
        u64 t_self = read_cntpct();
        while ((read_cntpct() - t_self) < 240000ULL) asm volatile("yield");
        u64 c1_after = sgi_counters[1][SGI_STAGE_HIDDEN];
        u64 c4_after = sgi_counters[4][SGI_STAGE_HIDDEN];
        u32 isp1_after = gicv3_read_ispendr0(1);
        u32 isp4_after = gicv3_read_ispendr0(4);
        u64 sc_after = g_gicv3_sgi_send_count;

        uart_puts(&console, "[SGI_TEST] send_count: 0x"); uart_put_hex(&console, sc_before);
        uart_puts(&console, " -> 0x");                     uart_put_hex(&console, sc_after);
        uart_puts(&console, " last_val=0x");               uart_put_hex(&console, g_gicv3_sgi_last_val);
        uart_puts(&console, "\r\n");
        uart_puts(&console, "[SGI_TEST] c1 ispendr0: 0x"); uart_put_hex(&console, isp1_before);
        uart_puts(&console, " -> imm 0x");                  uart_put_hex(&console, isp1_imm);
        uart_puts(&console, " -> after 0x");                uart_put_hex(&console, isp1_after);
        uart_puts(&console, " | irq_cnt: 0x");              uart_put_hex(&console, c1_before);
        uart_puts(&console, " -> 0x");                       uart_put_hex(&console, c1_after);
        uart_puts(&console, "\r\n");
        uart_puts(&console, "[SGI_TEST] c4 ispendr0: 0x"); uart_put_hex(&console, isp4_before);
        uart_puts(&console, " -> imm 0x");                  uart_put_hex(&console, isp4_imm);
        uart_puts(&console, " -> after 0x");                uart_put_hex(&console, isp4_after);
        uart_puts(&console, " | irq_cnt: 0x");              uart_put_hex(&console, c4_before);
        uart_puts(&console, " -> 0x");                       uart_put_hex(&console, c4_after);
        uart_puts(&console, "\r\n");
        // Verdict legend:
        //   ispendr_imm has bit1 set & irq_cnt+1 -> chain works
        //   ispendr_imm bit1 set & irq_cnt unchanged -> RD has SGI but PE not delivering (DAIF? VBAR?)
        //   ispendr_imm bit1 clear -> SGI never reached redistributor (sender / distributor)
    }
    
    // Phase 2: Start pipeline now that baseline is done.
    // Core 4/5 will transition from WFE workqueue mode to WFI+SGI pipeline mode.
    pipeit_start(&g_pipeit);
    LOG_INFO("Pipeline active — A72 mailbox/SEV fallback; SGI assist only if mode=1");
    
    LOG_INFO("Runtime: core0-first control loop active");
    
    // === Phase 2: Pipeline baseline benchmark ===
    // Now that IRQ is unmasked, SGI can reach core 4/5.
    // Submit frames and measure end-to-end latency + per-stage cycles.
    {
        const u32 PIPE_BENCH_COUNT = 1000;
        telemetry_t bench_tel;
        bench_tel.cpu_load = 0x10000;
        bench_tel.l2_latency_us = 0x20000;
        bench_tel.memory_pressure = 0x30000;
        bench_tel.thermal_state = 0x40000;
        bench_tel.packet_rate = 0x50000;
        bench_tel.node_count = 0x60000;
        
        // Reset pipeline profiling counters
        g_pipeit.hidden_cycles_sum = 0;
        g_pipeit.output_cycles_sum = 0;
        g_pipeit.hidden_count = 0;
        g_pipeit.output_count = 0;
        g_pipeit.total_frames = 0;
        // Reset completion bell. Bell is in NC region (PIPEIT_NC_BASE),
        // so the write goes straight to DRAM; no dc cvac needed. dsb ish
        // ensures the store is visible to A72 worker before bench starts.
        g_pipeit_completion_seq = 0;
        asm volatile("dsb ish" ::: "memory");

        u64 t_pipe_start = read_cntpct();

        u32 timeout_frames = 0;
        u32 submit_invalid = 0;
        u32 submit_oom = 0;
        u32 submit_other = 0;
        u32 submit_ok = 0;
        for (u32 i = 0; i < PIPE_BENCH_COUNT; i++) {
            u32 frame_id;
            result_t sub = pipeit_submit_frame(&g_pipeit, &bench_tel, &frame_id);
            if (sub == ERR_OUT_OF_MEMORY) {
                u64 t_retry = read_cntpct();
                while ((read_cntpct() - t_retry) < 1200000ULL) {
                    if (g_pipeit_completion_seq >= (u64)i) break;
                }
                sub = pipeit_submit_frame(&g_pipeit, &bench_tel, &frame_id);
            }

            if (sub != OK) {
                if (sub == ERR_INVALID_PARAM) submit_invalid++;
                else if (sub == ERR_OUT_OF_MEMORY) submit_oom++;
                else submit_other++;
                timeout_frames++;
                continue;
            }
            submit_ok++;

            u64 expected = (u64)(i + 1);
            u64 t0 = read_cntpct();
            u32 timed_out = 0;
            while (1) {
                if (g_pipeit_completion_seq >= expected) break;
                if ((read_cntpct() - t0) > 1200000ULL) { timed_out = 1; break; }
            }
            if (timed_out) timeout_frames++;

            if (((i + 1) % 100) == 0) {
                uart_puts(&console, "[PIPE_BENCH] progress=0x");
                uart_put_hex(&console, i + 1);
                uart_puts(&console, " completed=0x");
                uart_put_hex(&console, g_pipeit.total_frames);
                uart_puts(&console, " timeouts=0x");
                uart_put_hex(&console, timeout_frames);
                uart_puts(&console, "\r\n");
            }
        }
        
        u64 t_pipe_end = read_cntpct();
        u64 pipe_elapsed_ticks = t_pipe_end - t_pipe_start;
        u64 pipe_elapsed_us = pipe_elapsed_ticks / 24;
        u64 completed_frames = (u64)g_pipeit.total_frames;
        u64 pipe_avg_ns = completed_frames ?
            ((pipe_elapsed_ticks * 125) / (completed_frames * 3)) : 0;
        
        u64 avg_hidden_cyc = g_pipeit.hidden_count ? 
            g_pipeit.hidden_cycles_sum / g_pipeit.hidden_count : 0;
        u64 avg_output_cyc = g_pipeit.output_count ? 
            g_pipeit.output_cycles_sum / g_pipeit.output_count : 0;
        
        uart_puts(&console, "\r\n[PIPE_BENCH] Pipeline baseline (");
        uart_put_hex(&console, PIPE_BENCH_COUNT);
        uart_puts(&console, " frames)\r\n");
        uart_puts(&console, "[PIPE_BENCH] elapsed_us=0x");      uart_put_hex(&console, pipe_elapsed_us);
        uart_puts(&console, " avg_ns=0x");                       uart_put_hex(&console, pipe_avg_ns);
        uart_puts(&console, " completed=0x");                     uart_put_hex(&console, g_pipeit.total_frames);
        uart_puts(&console, "\r\n");
        uart_puts(&console, "[PIPE_BENCH] avg_hidden_cyc=0x");   uart_put_hex(&console, avg_hidden_cyc);
        uart_puts(&console, " avg_output_cyc=0x");               uart_put_hex(&console, avg_output_cyc);
        uart_puts(&console, "\r\n");
        
        // JSON output
        uart_puts(&console, "[PIPE_BENCH] === PIPE_JSON_BEGIN ===\r\n");
        uart_puts(&console, "{\"version\":\"v7\",\"mode\":\"pipeline\",\"n\":0x");  uart_put_hex(&console, PIPE_BENCH_COUNT);
        uart_puts(&console, ",\"elapsed_us\":0x");     uart_put_hex(&console, pipe_elapsed_us);
        uart_puts(&console, ",\"avg_ns\":0x");          uart_put_hex(&console, pipe_avg_ns);
        uart_puts(&console, ",\"completed\":0x");       uart_put_hex(&console, g_pipeit.total_frames);
        uart_puts(&console, ",\"avg_hidden_cyc\":0x");  uart_put_hex(&console, avg_hidden_cyc);
        uart_puts(&console, ",\"avg_output_cyc\":0x");  uart_put_hex(&console, avg_output_cyc);
        uart_puts(&console, "}\r\n[PIPE_BENCH] === PIPE_JSON_END ===\r\n");
        
        // === SGI diagnostic dump ===
        // hidden_irq_count == 0 -> SGI never reaches core 4 (GIC routing/redist issue)
        // hidden_irq_count > 0 but completed == 0 -> handler runs but bug inside
        extern volatile u64 sgi_counters[6][4];
        extern volatile u64 g_pipeit_sgi_sent[4];
        extern volatile u64 g_pipeit_hidden_handler_enter;
        extern volatile u64 g_pipeit_hidden_handler_exit;
        extern volatile u64 g_pipeit_output_handler_enter;
        extern volatile u64 g_pipeit_output_handler_exit;
        uart_puts(&console, "[PIPE_DIAG] timeouts=0x");      uart_put_hex(&console, timeout_frames);
        uart_puts(&console, " submit_ok=0x");               uart_put_hex(&console, submit_ok);
        uart_puts(&console, " submit_invalid=0x");          uart_put_hex(&console, submit_invalid);
        uart_puts(&console, " submit_oom=0x");              uart_put_hex(&console, submit_oom);
        uart_puts(&console, " submit_other=0x");            uart_put_hex(&console, submit_other);
        uart_puts(&console, " sgi_hidden_delta=0x");        uart_put_hex(&console,
            (g_pipeit_sgi_sent[SGI_STAGE_HIDDEN] >= (u64)submit_ok)
                ? (g_pipeit_sgi_sent[SGI_STAGE_HIDDEN] - (u64)submit_ok)
                : ((u64)submit_ok - g_pipeit_sgi_sent[SGI_STAGE_HIDDEN]));
        uart_puts(&console, "\r\n[PIPE_DIAG] sgi_sent: hidden=0x");           uart_put_hex(&console, g_pipeit_sgi_sent[SGI_STAGE_HIDDEN]);
        uart_puts(&console, " output=0x");                            uart_put_hex(&console, g_pipeit_sgi_sent[SGI_STAGE_OUTPUT]);
        uart_puts(&console, " done=0x");                              uart_put_hex(&console, g_pipeit_sgi_sent[SGI_STAGE_DONE]);
        extern volatile u64 g_pipeit_sgi_a72_enabled;
        uart_puts(&console, " mode_sgi_a72=0x");                       uart_put_hex(&console, g_pipeit_sgi_a72_enabled);
        uart_puts(&console, "\r\n[PIPE_DIAG] handler_hidden: enter=0x"); uart_put_hex(&console, g_pipeit_hidden_handler_enter);
        uart_puts(&console, " exit=0x");                                  uart_put_hex(&console, g_pipeit_hidden_handler_exit);
        uart_puts(&console, "\r\n[PIPE_DIAG] handler_output: enter=0x"); uart_put_hex(&console, g_pipeit_output_handler_enter);
        uart_puts(&console, " exit=0x");                                  uart_put_hex(&console, g_pipeit_output_handler_exit);
        // Poll-path diagnostic: poll_hits = SGIs picked up via IAR1 polling
        // (vs handler_*_enter which counts vector-routed deliveries).
        extern volatile u64 g_pipeit_poll_hits_hidden, g_pipeit_poll_hits_output;
        extern volatile u64 g_pipeit_iar_spurious_hidden, g_pipeit_iar_spurious_output;
        extern volatile u64 g_pipeit_last_iar_hidden, g_pipeit_last_iar_output;
        extern volatile u64 g_pipeit_loop_iter_hidden, g_pipeit_loop_iter_output;
        extern volatile u64 g_pipeit_last_hppir1_hidden, g_pipeit_last_rpr_hidden, g_pipeit_last_ispendr0_hidden;
        extern volatile u64 g_pipeit_last_hppir1_output, g_pipeit_last_rpr_output, g_pipeit_last_ispendr0_output;
        uart_puts(&console, "\r\n[PIPE_DIAG] poll_hidden: iter=0x"); uart_put_hex(&console, g_pipeit_loop_iter_hidden);
        uart_puts(&console, " hits=0x");                              uart_put_hex(&console, g_pipeit_poll_hits_hidden);
        uart_puts(&console, " spurious=0x");                          uart_put_hex(&console, g_pipeit_iar_spurious_hidden);
        uart_puts(&console, " last_iar=0x");                          uart_put_hex(&console, g_pipeit_last_iar_hidden);
        uart_puts(&console, "\r\n[PIPE_DIAG] hidden gic-self: hppir1=0x"); uart_put_hex(&console, g_pipeit_last_hppir1_hidden);
        uart_puts(&console, " rpr=0x");                                     uart_put_hex(&console, g_pipeit_last_rpr_hidden);
        uart_puts(&console, " ispendr0=0x");                                uart_put_hex(&console, g_pipeit_last_ispendr0_hidden);
        uart_puts(&console, "\r\n[PIPE_DIAG] poll_output: iter=0x"); uart_put_hex(&console, g_pipeit_loop_iter_output);
        uart_puts(&console, " hits=0x");                              uart_put_hex(&console, g_pipeit_poll_hits_output);
        uart_puts(&console, " spurious=0x");                          uart_put_hex(&console, g_pipeit_iar_spurious_output);
        uart_puts(&console, " last_iar=0x");                          uart_put_hex(&console, g_pipeit_last_iar_output);
        uart_puts(&console, "\r\n[PIPE_DIAG] output gic-self: hppir1=0x"); uart_put_hex(&console, g_pipeit_last_hppir1_output);
        uart_puts(&console, " rpr=0x");                                     uart_put_hex(&console, g_pipeit_last_rpr_output);
        uart_puts(&console, " ispendr0=0x");                                uart_put_hex(&console, g_pipeit_last_ispendr0_output);
        uart_puts(&console, "\r\n");
        for (u32 c = 0; c < 6; c++) {
            uart_puts(&console, "[PIPE_DIAG] sgi_irq core=0x"); uart_put_hex(&console, c);
            uart_puts(&console, " input=0x");  uart_put_hex(&console, sgi_counters[c][SGI_STAGE_INPUT]);
            uart_puts(&console, " hidden=0x"); uart_put_hex(&console, sgi_counters[c][SGI_STAGE_HIDDEN]);
            uart_puts(&console, " output=0x"); uart_put_hex(&console, sgi_counters[c][SGI_STAGE_OUTPUT]);
            uart_puts(&console, " done=0x");   uart_put_hex(&console, sgi_counters[c][SGI_STAGE_DONE]);
            uart_puts(&console, "\r\n");
        }
        // Per-core GIC state snapshot (recorded inside gicv3_init_cpu_iface).
        // init=0 -> function never ran on that core (PSCI never reached it,
        //          or smp_secondary_main returned early). 
        extern volatile u64 gicv3_core_diag[6][8];
        for (u32 c = 0; c < 6; c++) {
            uart_puts(&console, "[GIC_DIAG] core=0x");      uart_put_hex(&console, c);
            uart_puts(&console, " init=0x");                uart_put_hex(&console, gicv3_core_diag[c][0]);
            uart_puts(&console, " sre=0x");                 uart_put_hex(&console, gicv3_core_diag[c][1]);
            uart_puts(&console, " pmr=0x");                 uart_put_hex(&console, gicv3_core_diag[c][2]);
            uart_puts(&console, " igrpen1=0x");             uart_put_hex(&console, gicv3_core_diag[c][3]);
            uart_puts(&console, " waker=0x");               uart_put_hex(&console, gicv3_core_diag[c][4]);
            uart_puts(&console, " isen=0x");                uart_put_hex(&console, gicv3_core_diag[c][5]);
            uart_puts(&console, " igrp=0x");                uart_put_hex(&console, gicv3_core_diag[c][6]);
            uart_puts(&console, " typer_aff=0x");            uart_put_hex(&console, gicv3_core_diag[c][7]);
            uart_puts(&console, "\r\n");
        }
        // Extended GIC diag (rules out priority/group filters silently blocking IRQ).
        // igrpmodr=0 expected (Group 1 NS). ipri0/ipri1 should show 0xA0 bytes.
        // bpr1 default 0x4 means group preemption at bit[7:3]. ctlr defaults 0.
        // Column count MUST match hal/gicv3.c. Mismatched extern/definition
        // across translation units is undefined behaviour, not just a warning.
        extern volatile u64 gicv3_core_diag2[6][17];
        for (u32 c = 0; c < 6; c++) {
            uart_puts(&console, "[GIC_DIAG2] core=0x");      uart_put_hex(&console, c);
            uart_puts(&console, " igrpmodr0=0x");            uart_put_hex(&console, gicv3_core_diag2[c][0]);
            uart_puts(&console, " ipri0=0x");                uart_put_hex(&console, gicv3_core_diag2[c][1]);
            uart_puts(&console, " ipri1=0x");                uart_put_hex(&console, gicv3_core_diag2[c][2]);
            uart_puts(&console, " bpr1=0x");                 uart_put_hex(&console, gicv3_core_diag2[c][3]);
            uart_puts(&console, " ctlr=0x");                 uart_put_hex(&console, gicv3_core_diag2[c][4]);
            uart_puts(&console, " gicr_ctlr=0x");             uart_put_hex(&console, gicv3_core_diag2[c][5] & 0xFFFFFFFF);
            uart_puts(&console, " ap1r0=0x");                 uart_put_hex(&console, gicv3_core_diag2[c][5] >> 32);
            uart_puts(&console, " local_waker_flags=0x");     uart_put_hex(&console, gicv3_core_diag2[c][6]);
        uart_puts(&console, "\r\n");
        if (c >= 4) {
            u32 wf = (u32)(gicv3_core_diag2[c][6] >> 32);
            u32 wpre = (u32)gicv3_core_diag2[c][7];
            u32 wps1 = (u32)gicv3_core_diag2[c][8];
            u32 wca1 = (u32)gicv3_core_diag2[c][9];
            u32 wps0 = (u32)gicv3_core_diag2[c][10];
            u32 wca0 = (u32)gicv3_core_diag2[c][11];
            u32 wfin = (u32)gicv3_core_diag2[c][12];
            uart_puts(&console, "  A72_PROBE_A core=0x"); uart_put_hex(&console, c);
            uart_puts(&console, " wpre=0x");   uart_put_hex(&console, wpre);
            uart_puts(&console, " ps1_rb=0x"); uart_put_hex(&console, wps1);
            uart_puts(&console, " ca_polls_ps1=0x"); uart_put_hex(&console, wca1);
            uart_puts(&console, " ps0_rb=0x"); uart_put_hex(&console, wps0);
            uart_puts(&console, " ca_polls_ps0=0x"); uart_put_hex(&console, wca0);
            uart_puts(&console, " wfinal=0x");  uart_put_hex(&console, wfin);
            uart_puts(&console, "\r\n");
            uart_puts(&console, "  A72_PROBE_B core=0x"); uart_put_hex(&console, c);
            // wf is the high half of slot 6, which now carries hppir_after, so
            // printing both proves the probe plumbing end to end.
            uart_puts(&console, " slot6_hi=0x");     uart_put_hex(&console, wf);
            uart_puts(&console, " hppir_before=0x"); uart_put_hex(&console, gicv3_core_diag2[c][13]);
            uart_puts(&console, " hppir_after=0x");  uart_put_hex(&console, gicv3_core_diag2[c][14]);
            uart_puts(&console, " hppir_final=0x");  uart_put_hex(&console, gicv3_core_diag2[c][16]);
            uart_puts(&console, " verdict=");
            uart_puts(&console, gicv3_core_diag2[c][15] ? "PPI_DELIVERED" : "FORWARD_PATH_DEAD");
            uart_puts(&console, "\r\n");
        }
            uart_puts(&console, "\r\n");
        }
        // Decode DPG1NS bit (25) of GICR_CTLR for visual scan
        for (u32 c = 0; c < 6; c++) {
            u64 gicr_ctlr_v = gicv3_core_diag2[c][5] & 0xFFFFFFFF;
            uart_puts(&console, "[GIC_DIAG2] core=0x"); uart_put_hex(&console, c);
            uart_puts(&console, " DPG1NS=");           uart_put_hex(&console, (gicr_ctlr_v >> 25) & 1);
            uart_puts(&console, " DPG0=");             uart_put_hex(&console, (gicr_ctlr_v >> 24) & 1);
            uart_puts(&console, "\r\n");
        }
        // H-Exo: ADB400 big-cluster <-> GIC handshake, sampled at the four
        // bring-up stages. RK3399 carries GIC->PE interrupts over a separate
        // AXI4-Stream interface per cluster, so the A72 columns decide whether
        // that cluster's interrupt stream to the GIC block is up:
        // REQ_2GIC / REQ_GIC2 are the soft power requests, CLR_2GIC / CLR_GIC2
        // the hardware "clear" handshakes.
        {
            extern volatile u64 g_smp_adb_trace[4][2];
            uart_puts(&console, "\r\n");
            for (u32 st = 0; st < 4u; st++) {
                u32 con = (u32)(g_smp_adb_trace[st][0] & 0xFFFFFFFFu);
                u32 stb = (u32)(g_smp_adb_trace[st][1] & 0xFFFFFFFFu);
                uart_puts(&console, "  [ADB400] stage");
                uart_put_hex(&console, st);
                uart_puts(&console, " CON=0x"); uart_put_hex(&console, con);
                uart_puts(&console, " ST=0x");  uart_put_hex(&console, stb);
                uart_puts(&console, " | A72 con r2g=");
                uart_put_hex(&console, (con >> 5) & 1u);
                uart_puts(&console, " rg2=");
                uart_put_hex(&console, (con >> 6) & 1u);
                uart_puts(&console, " c2g=");
                uart_put_hex(&console, (con >> 13) & 1u);
                uart_puts(&console, " cg2=");
                uart_put_hex(&console, (con >> 14) & 1u);
                uart_puts(&console, " st r2g=");
                uart_put_hex(&console, (stb >> 5) & 1u);
                uart_puts(&console, " rg2=");
                uart_put_hex(&console, (stb >> 6) & 1u);
                uart_puts(&console, " c2g=");
                uart_put_hex(&console, (stb >> 13) & 1u);
                uart_puts(&console, " cg2=");
                uart_put_hex(&console, (stb >> 14) & 1u);
                uart_puts(&console, " | A53 con r2g=");
                uart_put_hex(&console, (con >> 2) & 1u);
                uart_puts(&console, " rg2=");
                uart_put_hex(&console, (con >> 3) & 1u);
                uart_puts(&console, "\r\n");
            }
        }
        // WAKER handshake trace from gicv3_force_wake_core():
        // pre -> after_ps1 -> after_ps0 -> final, retries, flags.
        // flags: bit0 sleep_timeout, bit1 wake_timeout,
        //        bit2 PS=1 write ignored, bit3 PS=0 write ignored.
        extern volatile u64 gicv3_waker_trace[6][6];
        for (u32 c = 0; c < 6; c++) {
            uart_puts(&console, "[WAKER_TRACE] core=0x");     uart_put_hex(&console, c);
            uart_puts(&console, " pre=0x");                   uart_put_hex(&console, gicv3_waker_trace[c][0]);
            uart_puts(&console, " ps1=0x");                   uart_put_hex(&console, gicv3_waker_trace[c][1]);
            uart_puts(&console, " ps0=0x");                   uart_put_hex(&console, gicv3_waker_trace[c][2]);
            uart_puts(&console, " final=0x");                 uart_put_hex(&console, gicv3_waker_trace[c][3]);
            uart_puts(&console, " retries=0x");               uart_put_hex(&console, gicv3_waker_trace[c][4]);
            uart_puts(&console, " flags=0x");                 uart_put_hex(&console, gicv3_waker_trace[c][5]);
            uart_puts(&console, "\r\n");
        }
        // Per-core PE state captured AFTER daifclr in smp_secondary_main.
        // enter=0     -> daifclr never executed (core stuck before WFE loop)
        // daif bit7=1 -> IRQ still masked (something re-set it)
        // vbar mismatch -> wrong vector table => IRQ goes to panic vector
        extern volatile u64 g_smp_pe_diag[6][8];
        for (u32 c = 0; c < 6; c++) {
            uart_puts(&console, "[PE_DIAG] core=0x");        uart_put_hex(&console, c);
            uart_puts(&console, " enter=0x");                uart_put_hex(&console, g_smp_pe_diag[c][0]);
            uart_puts(&console, " daif=0x");                 uart_put_hex(&console, g_smp_pe_diag[c][1]);
            uart_puts(&console, " vbar=0x");                 uart_put_hex(&console, g_smp_pe_diag[c][2]);
            uart_puts(&console, " mpidr=0x");                uart_put_hex(&console, g_smp_pe_diag[c][3]);
            uart_puts(&console, " hcr=0x");                  uart_put_hex(&console, g_smp_pe_diag[c][4]);
            uart_puts(&console, " cel=0x");                  uart_put_hex(&console, g_smp_pe_diag[c][5]);
            uart_puts(&console, " isr=0x");                  uart_put_hex(&console, g_smp_pe_diag[c][6]);
            uart_puts(&console, " sctlr=0x");                 uart_put_hex(&console, g_smp_pe_diag[c][7]);
            uart_puts(&console, "\r\n");
        }
        // Decode SCTLR_EL2: M=bit0 (MMU), C=bit2 (D-cache), I=bit12 (I-cache).
        // If M=0 or C=0 on cores 4/5 -> A72 reads bypass cache -> no CCI snoop
        // -> stale value seen for inter-cluster shared variables.
        for (u32 c = 0; c < 6; c++) {
            u64 s = g_smp_pe_diag[c][7];
            uart_puts(&console, "[PE_DIAG] core=0x"); uart_put_hex(&console, c);
            uart_puts(&console, " M=");              uart_put_hex(&console, s & 1);
            uart_puts(&console, " C=");              uart_put_hex(&console, (s >> 2) & 1);
            uart_puts(&console, " I=");              uart_put_hex(&console, (s >> 12) & 1);
            uart_puts(&console, "\r\n");
        }
        // Print expected VBAR for comparison
        u64 vbar_c0;
        asm volatile("mrs %0, vbar_el2" : "=r"(vbar_c0));
        uart_puts(&console, "[PE_DIAG] core0 vbar=0x"); uart_put_hex(&console, vbar_c0);
        uart_puts(&console, " (expected for all cores)\r\n");
        // Idle-loop trace per core. Tells us whether secondaries actually
        // wake from WFE, see g_pipeit_active=1, and enter pipeit_worker_idle_*.
        extern volatile u64 g_smp_loop_diag[6][4];
        extern volatile u32 g_pipeit_active;
        uart_puts(&console, "[LOOP_DIAG] g_pipeit_active(c0_view)=0x");
        uart_put_hex(&console, (u64)g_pipeit_active);
        uart_puts(&console, "\r\n");
        for (u32 c = 0; c < 6; c++) {
            uart_puts(&console, "[LOOP_DIAG] core=0x");      uart_put_hex(&console, c);
            uart_puts(&console, " iter=0x");                  uart_put_hex(&console, g_smp_loop_diag[c][0]);
            uart_puts(&console, " saw_active=0x");            uart_put_hex(&console, g_smp_loop_diag[c][1]);
            uart_puts(&console, " entered_worker=0x");        uart_put_hex(&console, g_smp_loop_diag[c][2]);
            uart_puts(&console, " last_active=0x");           uart_put_hex(&console, g_smp_loop_diag[c][3]);
            uart_puts(&console, "\r\n");
        }
    }

    uart_puts(&console, "\r\n[*] Network: IRQ-driven ARP + ICMP echo\r\n");
    LOG_INFO("IP: 192.168.1.10  |  try: ping 192.168.1.10");
    uart_puts(&console, "> ");
    static u8 rx_frame[1520];
    u64 next_runtime_tick = read_cntpct() + HEARTBEAT_CYCLES_24MHZ;
    while (1) {
        // Drain RX ring on IRQ flag (set by handle_irq_exception)
        if (gmac_rx_pending) {
            gmac_rx_pending = 0;
            usize rx_len;
            while (gmac_recv_raw(rx_frame, &rx_len) == OK && rx_len >= 14) {
                telemetry_note_packet(&telemetry);
                u16 etype = ((u16)rx_frame[12] << 8) | rx_frame[13];
                
                // Phase 5.1: H-Exo L2 protocol dispatch (EtherType 0x88EE)
                if (etype == HEXO_ETHERTYPE) {
                    hexo_l2_handle_rx(rx_frame, rx_len);
                    continue;
                }
                
                if (net_process(rx_frame, rx_len) == OK) {
                    if (etype == 0x0806) {
                        LOG_OK("NET: ARP reply sent");
                    } else if (etype == 0x0800) {
                        LOG_OK("NET: ICMP echo reply sent");
                        runtime_run_inference(true);
                    }
                } else {
                    uart_puts(&console, "[RX] 0x");
                    uart_put_hex(&console, etype);
                    uart_puts(&console, " len=");
                    uart_put_hex(&console, rx_len);
                    uart_puts(&console, "\r\n> ");
                }
            }
        }

        // Phase 5/6: Periodic ticks for distributed protocol modules
        hexo_offload_tick();
        gossip_tick(&g_gossip);
        peer_table_tick();
        thermal_guard_update(&g_thermal_guard);
        
        // Phase 6.3: Periodic beacon broadcast (1Hz)
        u64 cyc_now = read_cntpct();
        if ((i64)(cyc_now - g_next_beacon_cyc) >= 0) {
            hexo_beacon_t b;
            const u8* mac = gmac_get_mac();
            for (u32 i = 0; i < 6; i++) b.node_id[i] = mac[i];
            b.capabilities  = 0x1 | 0x4 | 0x8;  // NEON + PTP + Pipe-it
            b.cpu_load_pct  = telemetry.current.cpu_load;
            b.thermal_max   = (g_thermal_guard.cpu_temp > g_thermal_guard.gpu_temp)
                              ? g_thermal_guard.cpu_temp : g_thermal_guard.gpu_temp;
            b.free_slots    = PIPE_BUF_COUNT;
            b.inference_count = pmu_inference_count;
            hexo_l2_send_beacon(&b);
            g_next_beacon_cyc = cyc_now + BEACON_PERIOD_CYC;
        }
        
        u64 now = read_cntpct();
        if ((i64)(now - next_runtime_tick) >= 0) {
            runtime_update_loop_jitter(now);
            runtime_run_inference(true);
            next_runtime_tick += HEARTBEAT_CYCLES_24MHZ;
        }

        // UART (polled; wakes immediately after WFI)
        if (uart_rx_ready(&console)) {
            char c = uart_getc(&console);
            if (c == 'b' || c == 'B') {
                uart_puts(&console, "\r\n[*] Entering heartbeat benchmark mode\r\n");
                heartbeat_run(&console);
                heartbeat_stats_t hb_stats;
                heartbeat_get_stats(&hb_stats);
                adaptive_scheduler_update(&adaptive_sched, hb_stats.jitter_percent);
                uart_puts(&console, "[*] Returning to runtime loop\r\n> ");
                next_runtime_tick = read_cntpct() + HEARTBEAT_CYCLES_24MHZ;
                continue;
            }
            if (c == 's' || c == 'S') {
                uart_puts(&console, "\r\n");
                runtime_print_summary();
                uart_puts(&console, "> ");
                continue;
            }
            if (c == 'd' || c == 'D') {
                uart_puts(&console, "\r\n");
                smp_dump_diagnostics(&console);
                uart_puts(&console, "> ");
                continue;
            }
            if (c == 'r' || c == 'R') {
                uart_puts(&console, "\r\n[*] REBOOT via PSCI SYSTEM_RESET...\r\n");
                // PSCI SYSTEM_RESET: SMC #0 with w0 = 0x84000009 (PSCI_SYSTEM_RESET)
                asm volatile(
                    "mov w0, #0x0009\n"
                    "movk w0, #0x8400, lsl #16\n"
                    "smc #0\n"
                    ::: "w0", "memory"
                );
                while (1) asm volatile("wfe");  // should never reach here
            }
            if (c == '\r') uart_puts(&console, "\r\n> ");
            else           uart_putc(&console, c);
            continue;
        }
        // Idle with a bounded yield loop instead of WFI.
        //
        // Only the GMAC interrupt is routed in the GIC (SPI 24, enabled at
        // the GMAC init above), and no UART RX interrupt is enabled at all.
        // A byte that arrives while this core is in WFI therefore never
        // wakes it, so the polled console commands below stay unreachable
        // for as long as the board is idle. Measured 2026-10-02: with the
        // board sitting at the ">" prompt, sending "d" or "r" produced no
        // response at all, which also blocked unattended reboots.
        //
        // Spinning briefly keeps the console responsive at negligible cost.
        // The proper fix is to route and enable the UART2 GIC interrupt and
        // restore WFI; that needs the RK3399 UART2 IRQ id, which is not
        // present anywhere in this repository (no dts, no IRQ constant).
        for (u32 idle_spin = 0; idle_spin < 20000u; idle_spin++) {
            asm volatile("yield");
        }
    }
    
}
