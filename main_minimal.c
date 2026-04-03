// Minimal test kernel - just heartbeat, no chaos, no neural network
#include "hal/uart.h"

typedef unsigned int u32;
typedef unsigned long long u64;

static inline u64 read_cycles(void) {
    u64 val;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(val));
    return val;
}

void kmain(void) {
    uart_t console;
    uart_init(&console, UART2_BASE, 115200);
    
    uart_puts(&console, "\r\n========================================\r\n");
    uart_puts(&console, "  MINIMAL HEARTBEAT TEST\r\n");
    uart_puts(&console, "========================================\r\n\r\n");
    
    // Enable PMU
    u64 val = 1;
    asm volatile("msr pmcr_el0, %0" :: "r"(val));
    asm volatile("msr pmcntenset_el0, %0" :: "r"(val));
    
    u64 start = read_cycles();
    u64 last_beat = start;
    u32 beat_count = 0;
    
    uart_puts(&console, "[*] Starting heartbeat (every 100ms)...\r\n");
    uart_puts(&console, "[*] Press any key to exit\r\n\r\n");
    
    while (1) {
        u64 now = read_cycles();
        u64 delta = now - last_beat;
        
        // 150M cycles = 100ms @ 1.5GHz
        if (delta >= 150000000ULL) {
            beat_count++;
            
            // Simple output: BEAT N
            uart_puts(&console, "BEAT ");
            
            // Print number as hex
            char hex[9];
            u32 n = beat_count;
            for (int i = 7; i >= 0; i--) {
                int digit = n & 0xF;
                hex[i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
                n >>= 4;
            }
            hex[8] = '\0';
            uart_puts(&console, "0x");
            uart_puts(&console, hex);
            
            uart_puts(&console, " | Cycles: ");
            
            // Print cycles as hex
            u64 c = delta;
            char chex[17];
            for (int i = 15; i >= 0; i--) {
                int digit = c & 0xF;
                chex[i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
                c >>= 4;
            }
            chex[16] = '\0';
            uart_puts(&console, "0x");
            uart_puts(&console, chex);
            
            uart_puts(&console, "\r\n");
            
            last_beat = now;
        }
        
        // Check for keypress
        if (uart_rx_ready(&console)) {
            char c = uart_getc(&console);
            uart_puts(&console, "\r\n[*] Exiting...\r\n");
            break;
        }
        
        // Yield
        asm volatile("yield");
    }
    
    // Echo mode
    uart_puts(&console, "[*] Echo mode - type anything:\r\n> ");
    while (1) {
        char c = uart_getc(&console);
        if (c == '\r') {
            uart_puts(&console, "\r\n> ");
        } else {
            uart_putc(&console, c);
        }
    }
}
