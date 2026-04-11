#include <stdint.h>
#include "core/types.h"
#include "core/heartbeat.h"
#include "hal/gicv3.h"
#include "hal/cci.h"
#include "core/slab.h"
#include "hal/gmac.h"
#include "neuro/neuro_sync.h"
#include "neuro/telemetry.h"
#include "neuro/weight_validation.h"
#include "neuro/adaptive_scheduler.h"
#include "core/smp.h"
#include "core/log.h"
#include "core/workqueue.h"
#include "mmu.h"
#include "hal/net.h"

extern volatile u32 gmac_rx_pending;

#define UART2_BASE 0xFF1A0000
#define UART_THR   0x00
#define UART_USR   0x7C

uart_t console;
static neuro_sync_t neural_arbitrator;
static telemetry_collector_t telemetry;
static adaptive_scheduler_t adaptive_sched;
static inference_result_t last_inference_result;
static telemetry_t runtime_telemetry_snapshot;
static u64 runtime_last_tick_cycles;
static u32 runtime_loop_jitter_percent;
static bool runtime_last_offload_state;

// Runs on core 1 (A53) via work queue: one neural inference cycle per ICMP ping.
static void neuro_infer_worker(u64 arg) {
    (void)arg;
    adaptive_inference(&adaptive_sched, &telemetry.current, &last_inference_result);
}

static inline u64 read_cntpct(void) {
    u64 v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
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

static void runtime_run_inference(bool prefer_offload) {
    if (telemetry_collect(&telemetry, &runtime_telemetry_snapshot) != OK) {
        return;
    }

    u32 worker_core = smp_get_first_secondary_entered();
    bool offloaded = false;

    if (prefer_offload && worker_core != 0) {
        offloaded = (wq_try_dispatch(worker_core, neuro_infer_worker, 0) != 0);
    }

    if (!offloaded) {
        neuro_infer_worker(0);
    }

    if (offloaded != runtime_last_offload_state) {
        uart_puts(&console, "[RUNTIME] inference path -> ");
        uart_puts(&console, offloaded ? "secondary workqueue" : "core0 inline");
        uart_puts(&console, "\r\n");
        runtime_last_offload_state = offloaded;
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
    uart_puts(&console, "[OK] SMP: ");
    uart_put_hex(&console, smp_get_online_count());
    uart_puts(&console, " cores online\r\n");
    LOG_OK("WQ: Work queue initialized");
    dbg_putc_raw('>');
    dbg_putc_raw('W');
    dbg_putc_raw('<');
    dbg_putc_raw('\r');
    dbg_putc_raw('\n');

    uart_puts(&console, "[DBG] after WQ\r\n");
    smp_dump_diagnostics(&console);

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

    // Initialize Neural Arbitrator
    LOG_INFO("Initializing Neural Arbitrator...");
    result_t res = neuro_sync_init(&neural_arbitrator);
    if (res == OK) {
        LOG_OK("Neuro-Sync: TinyML Engine Ready");
        LOG_OK("Model: 6->8->4 Feedforward Network");
        LOG_OK("Arithmetic: Fixed-Point Q16.16");
        
        u32 expected_crc = get_expected_weights_crc();
        if (validate_weights_integrity(neural_arbitrator.weights, expected_crc)) {
            LOG_OK("Neural weights integrity verified");
        } else {
            LOG_WARN("Neural weights CRC mismatch (continuing anyway)");
        }

        // Initialize Adaptive Scheduler (EMA jitter feedback loop)
        adaptive_scheduler_init(&adaptive_sched, &neural_arbitrator, NULL);

        // Warmup inference - measure latency (use nominal values, telemetry_init not yet called)
        telemetry_t warmup_tel = { 50, 100, 20, 50, 0, 1 };
        inference_result_t warmup_result;
        u64 inf_start = read_cntpct();
        adaptive_inference(&adaptive_sched, &warmup_tel, &warmup_result);
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
    runtime_run_inference(true);
    LOG_INFO("Runtime: core0-first control loop active");
    uart_puts(&console, "[*] Commands: 'b' heartbeat bench, 's' runtime summary, 'd' SMP diagnostics\r\n");

    // Unmask IRQ at EL2 — from here GMAC RX fires handle_irq_exception
    asm volatile("msr daifclr, #2" ::: "memory");
    LOG_OK("IRQ: EL2 unmasked -- interrupt-driven network active");

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
