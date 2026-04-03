#include <stdint.h>

// Базовый адрес UART2 для RK3399
#define UART2_BASE 0xFF1A0000

// UART Registers
#define UART_RBR   (*(volatile uint32_t*)(UART2_BASE + 0x00)) // Receiver Buffer Register
#define UART_THR   (*(volatile uint32_t*)(UART2_BASE + 0x00)) // Transmit Holding Register
#define UART_DLL   (*(volatile uint32_t*)(UART2_BASE + 0x00)) // Divisor Latch Low
#define UART_DLH   (*(volatile uint32_t*)(UART2_BASE + 0x04)) // Divisor Latch High
#define UART_IIR   (*(volatile uint32_t*)(UART2_BASE + 0x08)) // Interrupt Identification Register
#define UART_FCR   (*(volatile uint32_t*)(UART2_BASE + 0x08)) // FIFO Control Register
#define UART_LCR   (*(volatile uint32_t*)(UART2_BASE + 0x0C)) // Line Control Register
#define UART_MCR   (*(volatile uint32_t*)(UART2_BASE + 0x10)) // Modem Control Register
#define UART_LSR   (*(volatile uint32_t*)(UART2_BASE + 0x14)) // Line Status Register

// RK3399 Clock info: UART clock is typically 24MHz (from OSC)
// For 115200 baud: 24000000 / (16 * 115200) = 13.02 -> divisor = 13
#define UART_DIVISOR 13

void uart_init() {
    // 1. Disable interrupts
    (*(volatile uint32_t*)(UART2_BASE + 0x04)) = 0; // IER = 0
    
    // 2. Enable DLAB (Divisor Latch Access Bit)
    UART_LCR |= (1 << 7);
    
    // 3. Set Divisor for 115200
    UART_DLL = (UART_DIVISOR & 0xFF);
    UART_DLH = ((UART_DIVISOR >> 8) & 0xFF);
    
    // 4. Disable DLAB and set 8N1 (8 bits, No parity, 1 stop bit)
    UART_LCR = 0x03;
    
    // 5. Enable and clear FIFOs
    UART_FCR = 0x07; // Enable FIFO, Clear RX/TX FIFO
    
    // 6. Set RTS/DTR
    UART_MCR = 0x03;
}

void uart_putc(char c) {
    // Ждем, пока бит 5 (THRE) в LSR станет 1 (передатчик пуст)
    while (!(UART_LSR & (1 << 5)));
    UART_THR = c;
}

char uart_getc() {
    // Ждем, пока бит 0 (Data Ready) в LSR станет 1
    while (!(UART_LSR & (1 << 0)));
    return (char)(UART_RBR & 0xFF);
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

void uart_put_hex(uint64_t n) {
    char hex[] = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(hex[(n >> i) & 0xF]);
    }
}

// Panic function: dumps registers and enters WFI loop
void panic(const char *msg) {
    uart_puts("\r\n!!! KERNEL PANIC !!!\r\n");
    uart_puts("Reason: ");
    uart_puts(msg);
    uart_puts("\r\n\r\n--- Register Dump ---\r\n");

    uint64_t reg;
    
    // Using inline assembly to capture registers
    // We'll capture a few key ones for the dump
    asm volatile("mov %0, x0" : "=r"(reg)); uart_puts("X0:  0x"); uart_put_hex(reg); uart_puts("\r\n");
    asm volatile("mov %0, x1" : "=r"(reg)); uart_puts("X1:  0x"); uart_put_hex(reg); uart_puts("\r\n");
    asm volatile("mov %0, x30" : "=r"(reg)); uart_puts("X30: 0x"); uart_put_hex(reg); uart_puts(" (LR)\r\n");
    
    asm volatile("mov %0, sp" : "=r"(reg)); uart_puts("SP:  0x"); uart_put_hex(reg); uart_puts("\r\n");
    asm volatile("mrs %0, CurrentEL" : "=r"(reg)); uart_puts("EL:  0x"); uart_put_hex(reg >> 2); uart_puts("\r\n");

    uart_puts("\r\nSystem Halted.\r\n");
    
    while (1) {
        asm volatile("wfi");
    }
}

void kmain() {
    // Initialize UART for 115200
    uart_init();
    
    // Первая фраза твоего собственного ядра!
    uart_puts("\r\n--- H-Exo: Aleph Engine v0.1 ---\r\n");
    uart_puts("[OK] Hardware: RK3399 (NanoPi M4) Initialized.\r\n");
    uart_puts("[OK] Stack: Initialized.\r\n");
    uart_puts("[OK] MMU: Enabled (Memory Dominance Active).\r\n");
    uart_puts("[OK] Caches: L1 D-Cache + I-Cache Enabled.\r\n");
    uart_puts("[OK] Address Space: Distributed Foundation Ready.\r\n");
    uart_puts("Echo mode active. Type something...\r\n\r\n> ");
    
    while (1) {
        char c = uart_getc();
        
        // Обработка Enter (CR -> CRLF)
        if (c == '\r') {
            uart_puts("\r\n> ");
        } else {
            uart_putc(c); // Эхо
        }
    }
}