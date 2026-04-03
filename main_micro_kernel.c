"// H-Exo Micro Kernel - Ultra-minimal version
// For maximum size reduction

#include <stdint.h>

// Minimal definitions
#define UART2_BASE 0xFF1A0000
#define UART_THR   0x00
#define UART_LSR   0x14
#define LSR_THRE   (1 << 5)
#define LSR_DR     (1 << 0)

// Minimal UART functions
static inline void uart_putc(char c) {
    volatile uint32_t* uart = (volatile uint32_t*)UART2_BASE;
    while (!(uart[UART_LSR >> 2] & LSR_THRE)) {}
    uart[UART_THR >> 2] = (uint32_t)c;
}

static inline void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

static inline char uart_getc(void) {
    volatile uint32_t* uart = (volatile uint32_t*)UART2_BASE;
    while (!(uart[UART_LSR >> 2] & LSR_DR)) {}
    return (char)(uart[UART_THR >> 2] & 0xFF);
}

static inline void delay(volatile int count) {
    while (count--) {
        __asm__ volatile(\"nop\");
    }
}

// Micro kernel main function
void kmain(void) {
    // Minimal banner
    uart_puts(\"H-Exo Micro v0.1\r\n\";
    
    // Simple loop
    uart_puts(\"Micro kernel ready\r\n\";
    uart_puts(\"> \";
    
    while (1) {
        char c = uart_getc();
        
        if (c == '\\r') {
            uart_puts(\"\r\n> \";
        } else if (c == 'h' || c == 'H') {
            // Simple heartbeat
            uart_puts(\"HEARTBEAT\";
            for (int i = 0; i < 5; i++) {
                uart_putc('.');
                delay(1000000);
            }
            uart_puts(\" OK\r\n> \";
        } else if (c == 'q' || c == 'Q') {
            uart_puts(\"\r\nQuitting...\r\n\";
            break;
        } else {
            uart_putc(c);
        }
    }
    
    uart_puts(\"Micro kernel exit\r\n\";
}

// Required for linking
void handle_sync_exception(void) {
    uart_puts(\"Exception!\r\n\";
    while (1) {}
}"