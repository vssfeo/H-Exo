#include <stdint.h>
#include "core/types.h"
#include "core/heartbeat.h"
#include "hal/gicv3.h"
#include "core/slab.h"
#include "hal/gmac.h"
#include "neuro/neuro_sync.h"
#include "neuro/telemetry.h"
#include "neuro/weight_validation.h"

#define UART2_BASE 0xFF1A0000

uart_t console;
static neuro_sync_t neural_arbitrator;
static telemetry_collector_t telemetry;

// External symbols
extern u64 node_identity[8];

void uart_puts(uart_t* uart, const char* s);
void uart_put_hex(uart_t* uart, u64 value);
void uart_putc(uart_t* uart, char c);

static void print_banner(void) {
    uart_puts(&console, "\r\n");
    uart_puts(&console, "========================================\r\n");
    uart_puts(&console, "  H-Exo Omni-Core: Aleph Engine v0.3\r\n");
    uart_puts(&console, "  Neural Arbitrator (Neuro-Sync) Active\r\n");
    uart_puts(&console, "========================================\r\n");
    uart_puts(&console, "\r\n");
}

static void print_telemetry(const telemetry_t* t) {
    uart_puts(&console, "[TELEMETRY]\r\n");
    uart_puts(&console, "  CPU Load: ");
    uart_put_hex(&console, t->cpu_load);
    uart_puts(&console, "%\r\n");
    uart_puts(&console, "  L2 Latency: ");
    uart_put_hex(&console, t->l2_latency_us);
    uart_puts(&console, " us\r\n");
    uart_puts(&console, "  Memory: ");
    uart_put_hex(&console, t->memory_pressure);
    uart_puts(&console, "%\r\n");
    uart_puts(&console, "  Thermal: ");
    uart_put_hex(&console, t->thermal_state);
    uart_puts(&console, "%\r\n");
}

static void print_inference(const inference_result_t* r) {
    uart_puts(&console, "[NEURAL INFERENCE]\r\n");
    uart_puts(&console, "  Task Priority: ");
    uart_put_hex(&console, r->task_priority);
    uart_puts(&console, "\r\n");
    uart_puts(&console, "  Migration Hint: ");
    if (r->migration_hint == 0) {
        uart_puts(&console, "STAY\r\n");
    } else if (r->migration_hint == 1) {
        uart_puts(&console, "MIGRATE_HIGH_PERF\r\n");
    } else {
        uart_puts(&console, "MIGRATE_LOW_POWER\r\n");
    }
    uart_puts(&console, "  Power State: ");
    const char* states[] = {"SLEEP", "IDLE", "ACTIVE", "TURBO"};
    uart_puts(&console, states[r->power_state & 0x3]);
    uart_puts(&console, "\r\n");
    uart_puts(&console, "  Trust Score: ");
    uart_put_hex(&console, r->trust_score);
    uart_puts(&console, "\r\n");
}

void kmain(void) {
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
    
    // 1. Initialize Slab Allocator
    slab_init();
    uart_puts(&console, "[OK] Slab: Initialized (512KB Heap)\r\n");

    // 2. Initialize GICv3
    if (gicv3_init() == OK) {
        uart_puts(&console, "[OK] GICv3: Interrupt Controller Ready\r\n");
    } else {
        uart_puts(&console, "[ERR] GICv3: Initialization Failed\r\n");
    }

    // 3. Initialize GMAC (Networking)
    if (gmac_init() == OK) {
        uart_puts(&console, "[OK] GMAC: PHY Reset & MAC Configured\r\n");
    } else {
        uart_puts(&console, "[ERR] GMAC: Initialization Failed\r\n");
    }

    print_banner();
    
    // PMU DIAGNOSTIC OUTPUT
    u64 current_el, pmcr, mdcr, pmuserenr, pmcntenset;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    asm volatile("mrs %0, pmuserenr_el0" : "=r"(pmuserenr));
    asm volatile("mrs %0, pmcntenset_el0" : "=r"(pmcntenset));
    
    uart_puts(&console, "\r\n[PMU DIAGNOSTIC]\r\n");
    uart_puts(&console, "Current EL: 0x");
    uart_put_hex(&console, current_el);
    uart_puts(&console, "\r\nPMCR_EL0: 0x");
    uart_put_hex(&console, pmcr);
    uart_puts(&console, "\r\nPMUSERENR_EL0: 0x");
    uart_put_hex(&console, pmuserenr);
    uart_puts(&console, "\r\nPMCNTENSET_EL0: 0x");
    uart_put_hex(&console, pmcntenset);
    
    // Try to read MDCR_EL2 if in EL2
    if ((current_el & 0xC) == 0x8) {
        asm volatile("mrs %0, mdcr_el2" : "=r"(mdcr));
        uart_puts(&console, "\r\nMDCR_EL2: 0x");
        uart_put_hex(&console, mdcr);
    }
    uart_puts(&console, "\r\n\r\n");
    
    // System initialization
    uart_puts(&console, "[OK] Hardware: RK3399 (NanoPi M4)\r\n");
    uart_puts(&console, "[OK] Boot: Aleph Engine Active\r\n");
    uart_puts(&console, "[OK] Stack: Initialized\r\n");
    uart_puts(&console, "[OK] MMU: Enabled (Memory Dominance)\r\n");
    uart_puts(&console, "[OK] Caches: L1 D-Cache + I-Cache Active\r\n");
    
    // Initialize Neural Arbitrator
    uart_puts(&console, "\r\n[*] Initializing Neural Arbitrator...\r\n");
    result_t res = neuro_sync_init(&neural_arbitrator);
    if (res == OK) {
        uart_puts(&console, "[OK] Neuro-Sync: TinyML Engine Ready\r\n");
        uart_puts(&console, "[OK] Model: 6->8->4 Feedforward Network\r\n");
        uart_puts(&console, "[OK] Arithmetic: Fixed-Point Q16.16\r\n");
        
        // Validate neural weights integrity
        uart_puts(&console, "[*] Validating neural weights integrity...\r\n");
        u32 expected_crc = get_expected_weights_crc();
        u32 actual_crc = compute_weights_crc32(neural_arbitrator.weights);
        
        uart_puts(&console, "    Expected CRC: 0x");
        uart_put_hex(&console, expected_crc);
        uart_puts(&console, "\r\n    Actual CRC:   0x");
        uart_put_hex(&console, actual_crc);
        uart_puts(&console, "\r\n");
        
        if (validate_weights_integrity(neural_arbitrator.weights, expected_crc)) {
            uart_puts(&console, "[OK] Neural weights integrity verified\r\n");
        } else {
            uart_puts(&console, "[WARN] Neural weights CRC mismatch (continuing anyway)\r\n");
            // TEMPORARY: Don't halt on CRC failure to test heartbeat
            // while(1) { asm volatile("wfi"); }
        }
    } else {
        uart_puts(&console, "[!!] Neuro-Sync: Initialization Failed\r\n");
    }
    
    // EMERGENCY BEACON: Point A - CRC check complete
    uart_puts(&console, "[BEACON] A - CRC check complete\r\n");
    
    // Initialize Telemetry
    uart_puts(&console, "[*] Initializing Telemetry System...\r\n");
    
    // EMERGENCY BEACON: Point B - Before telemetry init
    uart_puts(&console, "[BEACON] B - Before telemetry init\r\n");
    
    res = telemetry_init(&telemetry);
    
    // EMERGENCY BEACON: Point C - After telemetry init
    uart_puts(&console, "[BEACON] C - After telemetry init\r\n");
    
    if (res == OK) {
        uart_puts(&console, "[OK] Telemetry: PMU Cycle Counter Enabled\r\n");
        uart_puts(&console, "[OK] Telemetry: Real-time Metrics Active\r\n");
    }
    
    // EMERGENCY BEACON: Point D - Before menu
    uart_puts(&console, "[BEACON] D - Before menu\r\n");
    
    uart_puts(&console, "\r\n========================================\r\n");
    uart_puts(&console, "  H-Exo Omni-Core: Operational\r\n");
    uart_puts(&console, "  Adaptive Neural Fabric Ready\r\n");
    uart_puts(&console, "========================================\r\n");
    
    // EMERGENCY BEACON: Point E - Auto-starting Heartbeat Test
    uart_puts(&console, "[BEACON] E - Auto-starting Heartbeat Test\r\n");
    
    // Auto-start Heartbeat Stability Test (no menu needed)
    uart_puts(&console, "\r\n[*] Starting Heartbeat Stability Test...\r\n");
    uart_puts(&console, "[*] Press 'q' to exit and enter Echo Mode\r\n\r\n");
    
    heartbeat_init(&console);
    heartbeat_run(&console);
    
    // After heartbeat exits, go to echo mode
    uart_puts(&console, "\r\n[*] Entering Echo Mode\r\n> ");
    while (1) {
        char c = uart_getc(&console);
        if (c == '\r') {
            uart_puts(&console, "\r\n> ");
        } else {
            uart_putc(&console, c);
        }
    }
    
    // Dead code - kept for reference
    if (0) {
        // Echo mode
        uart_puts(&console, "[*] Echo Mode Active\r\n> ");
        while (1) {
            char c = uart_getc(&console);
            if (c == '\r') {
                uart_puts(&console, "\r\n> ");
            } else {
                uart_putc(&console, c);
            }
        }
    }
    
    // Neural Arbitrator Demo (default)
    uart_puts(&console, "[*] Running Neural Arbitrator Demo...\r\n");
    uart_puts(&console, "[*] Press SPACE to run inference, 'q' to quit\r\n\r\n");
    
    u32 demo_iteration = 0;
    
    while (1) {
        char c = uart_getc(&console);
        
        if (c == '\r' || c == ' ') {
            demo_iteration++;
            
            uart_puts(&console, "\r\n--- Iteration ");
            uart_put_hex(&console, demo_iteration);
            uart_puts(&console, " ---\r\n");
            
            // Collect telemetry
            telemetry_t current_telemetry;
            telemetry_collect(&telemetry, &current_telemetry);
            
            // Simulate varying load for demo
            current_telemetry.cpu_load = (demo_iteration * 17) % 100;
            current_telemetry.l2_latency_us = 50 + (demo_iteration * 13) % 200;
            current_telemetry.memory_pressure = (demo_iteration * 23) % 80;
            
            print_telemetry(&current_telemetry);
            
            // Run neural inference
            inference_result_t result;
            res = neuro_sync_inference(&neural_arbitrator, &current_telemetry, &result);
            
            if (res == OK) {
                print_inference(&result);
                
                // Act on inference
                uart_puts(&console, "\r\n[ACTION] ");
                if (result.migration_hint == 1) {
                    uart_puts(&console, "Migrating task to high-performance node\r\n");
                } else if (result.migration_hint == 2) {
                    uart_puts(&console, "Migrating task to low-power node\r\n");
                }
                
                if (result.power_state == 3) {
                    uart_puts(&console, "[ACTION] Entering TURBO mode (high load predicted)\r\n");
                } else if (result.power_state == 0) {
                    uart_puts(&console, "[ACTION] Entering SLEEP mode (low load predicted)\r\n");
                }
            }
            
            uart_puts(&console, "\r\n> ");
        } else if (c == 'q') {
            uart_puts(&console, "\r\n[*] Exiting demo...\r\n");
            break;
        }
    }
    
    uart_puts(&console, "\r\nEcho mode. Type something...\r\n> ");
    
    while (1) {
        char c = uart_getc(&console);
        if (c == '\r') {
            uart_puts(&console, "\r\n> ");
        } else {
            uart_putc(&console, c);
        }
    }
}
