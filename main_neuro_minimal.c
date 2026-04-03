"#include <stdint.h>
#include \"core/types.h\"
#include \"core/heartbeat.h\"
#include \"hal/uart.h\"
#include \"neuro/neuro_sync.h\"
#include \"neuro/telemetry.h\"
#include \"neuro/weight_validation.h\"

#define UART2_BASE 0xFF1A0000

uart_t console;
extern u64 node_identity[8];

// Forward declarations
static void minimal_banner(void);
static void minimal_system_info(void);

//==============================================================================
// Minimal Banner
//==============================================================================
static void minimal_banner(void) {
#ifdef MINIMAL_OUTPUT
    uart_puts(&console, \"H-Exo Mini v0.1\r\n\";
#else
    uart_puts(&console, \"\r\n========================================\r\n\";
    uart_puts(&console, \"  H-Exo Omni-Core: Aleph Engine v0.3\r\n\";
    uart_puts(&console, \"  Neural Arbitrator OPTIMIZED\r\n\";
    uart_puts(&console, \"========================================\r\n\";
#endif
}

//==============================================================================
// Minimal System Info
//==============================================================================
static void minimal_system_info(void) {
#ifdef MINIMAL_OUTPUT
    uart_puts(&console, \"RK3399 OK\r\n\";
#else
    uart_puts(&console, \"[OK] Hardware: RK3399 (NanoPi M4)\r\n\";
    uart_puts(&console, \"[OK] Boot: Aleph Engine Active\r\n\";
#endif
}

//==============================================================================
// Minimal Kernel Entry Point
//==============================================================================
void kmain(void) {
    // Initialize UART with minimal config
    uart_config_t uart_cfg = {
        .base_addr = UART2_BASE,
        .baud_rate = 115200,  // Reduced from 1500000 for stability
        .data_bits = 8,
        .stop_bits = 1,
        .parity = 0,
        .fifo_depth = 16
    };
    uart_init(&console, &uart_cfg);
    
    // Minimal banner
    minimal_banner();
    
    // Minimal system info
    minimal_system_info();
    
#ifndef MINIMAL_OUTPUT
    // Neural Arbitrator initialization
    static neuro_sync_t neural_arbitrator;
    if (neuro_sync_init(&neural_arbitrator) == OK) {
        uart_puts(&console, \"[OK] Neuro-Sync Ready\r\n\";
        
        // Validate neural weights integrity
        u32 expected_crc = get_expected_weights_crc();
        u32 actual_crc = compute_weights_crc32(neural_arbitrator.weights);
        
        if (validate_weights_integrity(neural_arbitrator.weights, expected_crc)) {
            uart_puts(&console, \"[OK] Weights verified\r\n\";
        } else {
            uart_puts(&console, \"[WARN] Weights CRC mismatch\r\n\";
        }
    }
    
    uart_puts(&console, \"\r\nMinimal mode active.\r\n\";
#endif
    
    // Ultra-minimal loop - just heartbeat
    heartbeat_init(&console);
    heartbeat_run(&console);
    
    // Fallback echo mode
    uart_puts(&console, \"Echo mode. Type something...\r\n> \";
    while (1) {
        char c = uart_getc(&console);
        if (c == '\\r') {
            uart_puts(&console, \"\r\n> \";
        } else if (c == 0x03) {  // Ctrl+C
            uart_puts(&console, \"\r\n[*] Interrupt\r\n> \";
        } else {
            uart_putc(&console, c);
        }
    }
}"