// H-Exo Omni-Core: UART Implementation
// Optimized with inline assembly for critical paths

#include "uart.h"

// RK3399 UART2 register offsets
#define UART_RBR    0x00
#define UART_THR    0x00
#define UART_DLL    0x00
#define UART_DLH    0x04
#define UART_IER    0x04
#define UART_FCR    0x08
#define UART_LCR    0x0C
#define UART_MCR    0x10
#define UART_LSR    0x14

#define LSR_THRE    (1 << 5)
#define LSR_DR      (1 << 0)
#define LCR_DLAB    (1 << 7)

// Optimized register access with memory barriers
static inline void uart_write_reg(volatile u32* base, u32 offset, u32 value) {
    *(base + (offset >> 2)) = value;
    asm volatile("dmb sy" ::: "memory");
}

static inline u32 uart_read_reg(volatile u32* base, u32 offset) {
    asm volatile("dmb sy" ::: "memory");
    return *(base + (offset >> 2));
}

result_t uart_init(uart_t* uart, const uart_config_t* config) {
    if (!uart || !config) return ERR_INVALID_PARAM;
    
    uart->base = (volatile u32*)(uintptr_t)config->base_addr;
    uart->config = *config;
    
    // Calculate divisor for baud rate (24MHz clock)
    u32 divisor = 24000000 / (16 * config->baud_rate);
    
    // Disable interrupts
    uart_write_reg(uart->base, UART_IER, 0);
    
    // Enable DLAB
    uart_write_reg(uart->base, UART_LCR, LCR_DLAB);
    
    // Set divisor
    uart_write_reg(uart->base, UART_DLL, divisor & 0xFF);
    uart_write_reg(uart->base, UART_DLH, (divisor >> 8) & 0xFF);
    
    // 8N1, disable DLAB
    uart_write_reg(uart->base, UART_LCR, 0x03);
    
    // Enable and clear FIFOs
    uart_write_reg(uart->base, UART_FCR, 0x07);
    
    // Set RTS/DTR
    uart_write_reg(uart->base, UART_MCR, 0x03);
    
    uart->initialized = true;
    return OK;
}

void uart_putc(uart_t* uart, char c) {
    // Optimized busy-wait with CPU hint
    while (!(uart_read_reg(uart->base, UART_LSR) & LSR_THRE)) {
        asm volatile("yield");
    }
    uart_write_reg(uart->base, UART_THR, (u32)c);
}

char uart_getc(uart_t* uart) {
    while (!(uart_read_reg(uart->base, UART_LSR) & LSR_DR)) {
        asm volatile("yield");
    }
    return (char)(uart_read_reg(uart->base, UART_RBR) & 0xFF);
}

void uart_puts(uart_t* uart, const char* s) {
    while (*s) {
        uart_putc(uart, *s++);
    }
}

void uart_put_hex(uart_t* uart, u64 value) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(uart, hex[(value >> i) & 0xF]);
    }
}

bool uart_rx_ready(uart_t* uart) {
    return (uart_read_reg(uart->base, UART_LSR) & LSR_DR) != 0;
}

bool uart_tx_ready(uart_t* uart) {
    return (uart_read_reg(uart->base, UART_LSR) & LSR_THRE) != 0;
}

// DMA support stubs (to be implemented with DMAC driver)
result_t uart_dma_tx(uart_t* uart, const void* data, usize len) {
    // TODO: Implement DMA transfer for high-throughput L2 mesh
    return ERR_NOT_FOUND;
}

result_t uart_dma_rx(uart_t* uart, void* buffer, usize len) {
    // TODO: Implement DMA receive
    return ERR_NOT_FOUND;
}
