// H-Exo Omni-Core: Heartbeat Stability Monitor Implementation

#include "../hal/uart.h"
#include "heartbeat.h"
#include "chaos.h"

static heartbeat_stats_t stats;
static uart_t* uart_console = 0;
static bool chaos_mode_enabled = false;

// Read Generic Timer (CNTPCT_EL0) - always available, not blocked by BL31
// RK3399 Generic Timer runs at 24 MHz (41.67 ns per tick)
static inline u64 read_cycles(void) {
    u64 val;
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

void heartbeat_init(uart_t* uart) {
    uart_console = uart;
    
    uart_puts(uart, "[*] Heartbeat: Using Generic Timer (cntpct_el0 @ 24MHz)...\r\n");
    
    // cntpct_el0 is always accessible - no setup needed
    u64 initial_cycles = read_cycles();
    uart_puts(uart, "[*] Heartbeat: Initial cycles = 0x");
    uart_put_hex(uart, initial_cycles);
    uart_puts(uart, "\r\n");
    
    stats.beat_count = 0;
    stats.total_cycles = 0;
    stats.min_interval_cycles = 0xFFFFFFFFFFFFFFFFULL;
    stats.max_interval_cycles = 0;
    stats.last_beat_cycles = initial_cycles;
    stats.jitter_percent = 0;
    
    uart_puts(uart, "[OK] Heartbeat: Generic Timer ready\r\n");
}

void heartbeat_run(uart_t* uart) {
    uart_console = uart;
    
    uart_puts(uart, "\r\n[*] Entering Heartbeat Mode (Generic Timer)\r\n");
    uart_puts(uart, "[*] Interval: 100ms (2.4M ticks @ 24MHz)\r\n");
    uart_puts(uart, "[*] Press 'q' to exit\r\n\r\n");
    
    while (1) {
        u64 now = read_cycles();
        u64 delta = now - stats.last_beat_cycles;
        
        // Check if interval elapsed
        if (delta >= HEARTBEAT_CYCLES_24MHZ) {
            stats.beat_count++;
            stats.total_cycles += delta;
            
            // Update min/max
            if (delta < stats.min_interval_cycles) {
                stats.min_interval_cycles = delta;
            }
            if (delta > stats.max_interval_cycles) {
                stats.max_interval_cycles = delta;
            }
            
            // Calculate jitter
            i64 deviation = (i64)delta - (i64)HEARTBEAT_CYCLES_24MHZ;
            if (deviation < 0) deviation = -deviation;
            u32 jitter = (u32)((deviation * 100) / HEARTBEAT_CYCLES_24MHZ);
            if (jitter > stats.jitter_percent) {
                stats.jitter_percent = jitter;
            }
            
            // Output heartbeat
            uart_puts(uart, "BEAT #");
            uart_put_hex(uart, stats.beat_count);
            uart_puts(uart, " | Cycles: ");
            uart_put_hex(uart, delta);
            uart_puts(uart, " | Jitter: ");
            uart_put_hex(uart, jitter);
            uart_puts(uart, "%\r\n");
            
            stats.last_beat_cycles = now;
            
            // Chaos mode: inject instability every 10 beats
            if (chaos_mode_enabled && (stats.beat_count % 10 == 0)) {
                uart_puts(uart, "[CHAOS] Injecting instability...\r\n");
                chaos_apply(CHAOS_NOP_STORM, 50, 10);
            }
            
            // Print stats every 10 beats
            if (stats.beat_count % 10 == 0) {
                heartbeat_print_stats(uart);
            }
        }
        
        // Check for 'q' to exit
        if (uart_rx_ready(uart)) {
            char c = uart_getc(uart);
            if (c == 'q' || c == 'Q' || c == 0x03) {
                uart_puts(uart, "\r\n[*] Heartbeat stopped by user\r\n");
                heartbeat_print_stats(uart);
                break;
            }
        }
        
        // Yield CPU
        asm volatile("yield");
    }
}

void heartbeat_get_stats(heartbeat_stats_t* out) {
    if (out) {
        *out = stats;
    }
}

void heartbeat_print_stats(uart_t* uart) {
    uart_puts(uart, "\r\n--- Heartbeat Statistics ---\r\n");
    uart_puts(uart, "Total beats: ");
    uart_put_hex(uart, stats.beat_count);
    uart_puts(uart, "\r\n");
    
    uart_puts(uart, "Min interval: ");
    uart_put_hex(uart, stats.min_interval_cycles);
    uart_puts(uart, " cycles\r\n");
    
    uart_puts(uart, "Max interval: ");
    uart_put_hex(uart, stats.max_interval_cycles);
    uart_puts(uart, " cycles\r\n");
    
    uart_puts(uart, "Max jitter: ");
    uart_put_hex(uart, stats.jitter_percent);
    uart_puts(uart, "%\r\n");
    
    if (stats.beat_count > 0) {
        u64 avg = stats.total_cycles / stats.beat_count;
        uart_puts(uart, "Avg interval: ");
        uart_put_hex(uart, avg);
        uart_puts(uart, " cycles\r\n");
    }
    
    if (chaos_mode_enabled) {
        uart_puts(uart, "Chaos mode: ACTIVE\r\n");
    }
    
    uart_puts(uart, "----------------------------\r\n\r\n");
}

void heartbeat_enable_chaos(bool enable) {
    chaos_mode_enabled = enable;
    if (enable) {
        chaos_init();
    } else {
        chaos_stop();
    }
}
