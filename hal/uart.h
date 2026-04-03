// H-Exo Omni-Core: UART Hardware Abstraction Layer
// Optimized for RK3399 UART2 with DMA preparation

#ifndef HEXO_HAL_UART_H
#define HEXO_HAL_UART_H

#include "../core/types.h"

// UART configuration
typedef struct {
    u32 base_addr;
    u32 baud_rate;
    u8  data_bits;
    u8  stop_bits;
    u8  parity;
    u8  fifo_depth;
} uart_config_t;

// UART handle
typedef struct {
    volatile u32* base;
    uart_config_t config;
    bool initialized;
} uart_t;

// API
result_t uart_init(uart_t* uart, const uart_config_t* config);
void     uart_putc(uart_t* uart, char c);
char     uart_getc(uart_t* uart);
void     uart_puts(uart_t* uart, const char* s);
void     uart_put_hex(uart_t* uart, u64 value);
bool     uart_rx_ready(uart_t* uart);
bool     uart_tx_ready(uart_t* uart);

// DMA support (future)
result_t uart_dma_tx(uart_t* uart, const void* data, usize len);
result_t uart_dma_rx(uart_t* uart, void* buffer, usize len);

#endif // HEXO_HAL_UART_H
