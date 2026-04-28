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
    u64 clk_target_mhz;
    u64 avg_cycles, min_cycles, max_cycles;
    u64 avg_inst, avg_l1m, avg_l2m, avg_bus, avg_br;
    u64 avg_asimd;         // ASIMD_SPEC: NEON ops speculatively executed (A72)
    u64 avg_ticks, min_ticks, max_ticks, var_ticks;
    u64 avg_ns, elapsed_us;
    u64 ipc_x1000, cpu_mhz;
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

// Runs on A72 core 4 via wq_dispatch. Mirrors the A53 baseline loop in kmain
// so the only delta vs v2 is "which cluster executed". PMU registers are
// banked per-core so we must call pmu_init_local() before taking snapshots.
static void baseline_runner_a72(u64 arg) {
    (void)arg;

    // Raise A72 cluster B clock from fallback OPP (~408 MHz) to 1608 MHz
    // via direct ABPLL writes. During the call this CPU briefly runs from
    // OSC (24 MHz) while the PLL relocks.
    g_a72_baseline.clk_target_mhz = 1608;
    g_a72_baseline.clk_set_ret    = (i64)cru_set_a72_freq_mhz(1608);
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
    u64 t0 = read_cntpct();
    for (u32 it = 0; it < PMU_BASELINE_COUNT; it++) {
        u64 ts0 = read_cntpct();
        pmu_take_snapshot(&b_start);
        adaptive_inference_a72(&adaptive_sched, &g_a72_warmup_tel, &r);
        pmu_take_snapshot(&b_end);
        u64 dt = read_cntpct() - ts0;
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

    // v19 DIAGNOSTIC: Read I2C0 registers from A53 core (core 0) BEFORE any init
    // to determine if the 0x65 phenomenon is system-wide or A72-specific.
    {
        volatile u32 *i2c0 = (volatile u32 *)(uintptr_t)0xFF3C0000UL;
        uart_puts(&console, "[I2C0_DIAG_A53] IC_CON=0x");
        uart_put_hex(&console, i2c0[0x00 >> 2]);
        uart_puts(&console, " IC_STATUS=0x");
        uart_put_hex(&console, i2c0[0x70 >> 2]);
        uart_puts(&console, " IC_ENABLE=0x");
        uart_put_hex(&console, i2c0[0x6C >> 2]);
        uart_puts(&console, " IC_ENABLE_STATUS=0x");
        uart_put_hex(&console, i2c0[0x9C >> 2]);
        uart_puts(&console, " IC_TXFLR=0x");
        uart_put_hex(&console, i2c0[0x74 >> 2]);
        uart_puts(&console, " IC_RXFLR=0x");
        uart_put_hex(&console, i2c0[0x78 >> 2]);
        uart_puts(&console, " IC_TX_ABRT=0x");
        uart_put_hex(&console, i2c0[0x80 >> 2]);
        uart_puts(&console, "\r\n");
    }

    // Switch TTBR0_EL2 to our tables: DRAM=Normal WB, MMIO=Device-nGnRnE
    mmu_init();
    mmu_enable();
    LOG_OK("MMU: EL2 identity map active");

    // 1. Initialize Slab Allocator
    slab_init();
    LOG_OK("Slab: Initialized (512KB Heap)");

    // 1.5 Enable CCI-500 coherency before any secondary core bring-up.
    if (cci500_enable() == OK) {
        LOG_OK("CCI-500: snoop+DVM enabled");
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
    LOG_OK("GICv3: All redistributors pre-woken (ProcessorSleep=0)");
    smp_init();
    wq_init();

    // 3. Initialize GICv3 (after secondary cores are running)
    if (gicv3_init() == OK) {
        LOG_OK("GICv3: Interrupt Controller Ready");
    } else {
        LOG_ERR("GICv3: Initialization Failed");
    }
    // Core 0's per-PE GIC state (SGI enable/group/priority on its own
    // redistributor) is NOT touched by gicv3_init(); it must run the same
    // per-core init the secondaries do, otherwise SGI_STAGE_DONE from core 5
    // can never be received here, and a self-SGI test from core 0 to core 0
    // will silently drop.
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
                LOG_OK("Mali-T860: Smoke test PASSED (NULL job submission works)");
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
        u64 baseline_t0 = read_cntpct();
        for (u32 it = 0; it < PMU_BASELINE_COUNT; it++) {
            u64 ts0 = read_cntpct();
            pmu_take_snapshot(&b_start);
            inference_result_t r;
            adaptive_inference(&adaptive_sched, &warmup_tel, &r);
            pmu_take_snapshot(&b_end);
            u64 dt = read_cntpct() - ts0;
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
        
        // Wait up to 30s for A72 to finish
        u64 wait_t0 = read_cntpct();
        const u64 wait_timeout = 24ULL * 1000 * 1000 * 30;
        while (g_a72_baseline.done_flag == 0) {
            if (read_cntpct() - wait_t0 > wait_timeout) break;
            asm volatile("yield");
        }
        asm volatile("dmb ish" ::: "memory");

        if (g_a72_baseline.done_flag) {
            uart_puts(&console, "[BASELINE_A72] clk_set_ret=0x");    uart_put_hex(&console, (u64)g_a72_baseline.clk_set_ret);
            uart_puts(&console, "\r\n[BASELINE_A72] avg_cycles=0x");  uart_put_hex(&console, g_a72_baseline.avg_cycles);
            uart_puts(&console, " avg_ns=0x");                        uart_put_hex(&console, g_a72_baseline.avg_ns);
            uart_puts(&console, " ipc=0x");                           uart_put_hex(&console, g_a72_baseline.ipc_x1000);
            uart_puts(&console, " cpu_mhz=0x");                      uart_put_hex(&console, g_a72_baseline.cpu_mhz);
            uart_puts(&console, " avg_asimd=0x");                    uart_put_hex(&console, g_a72_baseline.avg_asimd);
            uart_puts(&console, "\r\n[BASELINE_A72] === BASELINE_JSON_BEGIN ===\r\n");
            uart_puts(&console, "{\"version\":\"v6\",\"timer\":\"pmccntr+cntpct\",\"core\":\"A72\",\"mpidr\":0x");
            uart_put_hex(&console, g_a72_baseline.mpidr);
            uart_puts(&console, ",\"clk_set_ret\":0x");    uart_put_hex(&console, (u64)g_a72_baseline.clk_set_ret);
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
    LOG_INFO("Pipeline active — core 4 (hidden), core 5 (output) in WFI+SGI mode");
    
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
        
        u64 t_pipe_start = read_cntpct();
        
        u32 timeout_frames = 0;
        for (u32 i = 0; i < PIPE_BENCH_COUNT; i++) {
            u32 frame_id;
            pipeit_submit_frame(&g_pipeit, &bench_tel, &frame_id);
            
            // Wait for frame to complete (poll stage_complete)
            // dispatch_idx is the actual buffer slot index (not frame_id counter)
            u32 fidx = g_pipe_buffer.dispatch_idx;
            u64 t0 = read_cntpct();
            int timed_out = 0;
            while (!(g_pipe_buffer.frames[fidx].stage_complete & (1u << PIPE_STAGE_OUTPUT))) {
                asm volatile("yield");
                if ((read_cntpct() - t0) > 1200000ULL) { timed_out = 1; break; }  // 50ms
            }
            if (timed_out) timeout_frames++;
            
            // Progress log every 100 frames
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
        u64 pipe_avg_ns = (pipe_elapsed_ticks * 125) / (PIPE_BENCH_COUNT * 3);
        
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
        uart_puts(&console, "\r\n[PIPE_DIAG] sgi_sent: hidden=0x");  uart_put_hex(&console, g_pipeit_sgi_sent[SGI_STAGE_HIDDEN]);
        uart_puts(&console, " output=0x");                            uart_put_hex(&console, g_pipeit_sgi_sent[SGI_STAGE_OUTPUT]);
        uart_puts(&console, " done=0x");                              uart_put_hex(&console, g_pipeit_sgi_sent[SGI_STAGE_DONE]);
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
        extern volatile u64 gicv3_core_diag2[6][6];
        for (u32 c = 0; c < 6; c++) {
            uart_puts(&console, "[GIC_DIAG2] core=0x");      uart_put_hex(&console, c);
            uart_puts(&console, " igrpmodr0=0x");            uart_put_hex(&console, gicv3_core_diag2[c][0]);
            uart_puts(&console, " ipri0=0x");                uart_put_hex(&console, gicv3_core_diag2[c][1]);
            uart_puts(&console, " ipri1=0x");                uart_put_hex(&console, gicv3_core_diag2[c][2]);
            uart_puts(&console, " bpr1=0x");                 uart_put_hex(&console, gicv3_core_diag2[c][3]);
            uart_puts(&console, " ctlr=0x");                 uart_put_hex(&console, gicv3_core_diag2[c][4]);
            uart_puts(&console, " gicr_ctlr=0x");             uart_put_hex(&console, gicv3_core_diag2[c][5] & 0xFFFFFFFF);
            uart_puts(&console, " ap1r0=0x");                 uart_put_hex(&console, gicv3_core_diag2[c][5] >> 32);
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
        asm volatile("wfi"); // sleep until next GMAC IRQ or SEV
    }
    
}
