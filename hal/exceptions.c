#include "../core/types.h"
#include "uart.h"
#include "gicv3.h"
#include "gmac.h"

// Set by IRQ handler; cleared + drained by main loop.
volatile u32 gmac_rx_pending = 0;

// Context structure saved by vectors.s
// CRITICAL: Must match SAVE_CONTEXT layout exactly!
// Layout: x0-x30 (31 regs * 8 = 248 bytes) in 256-byte frame
typedef struct {
    u64 x0;
    u64 x1;
    u64 x2;
    u64 x3;
    u64 x4;
    u64 x5;
    u64 x6;
    u64 x7;
    u64 x8;
    u64 x9;
    u64 x10;
    u64 x11;
    u64 x12;
    u64 x13;
    u64 x14;
    u64 x15;
    u64 x16;
    u64 x17;
    u64 x18;
    u64 x19;
    u64 x20;
    u64 x21;
    u64 x22;
    u64 x23;
    u64 x24;
    u64 x25;
    u64 x26;
    u64 x27;
    u64 x28;
    u64 x29;
    u64 x30;
} exception_context_t;

static void dump_regs(exception_context_t* ctx) {
    extern uart_t console;
    u64* regs = (u64*)ctx;
    
    uart_puts(&console, "\r\n=== EXCEPTION CONTEXT ===\r\n");
    
    // Dump all 31 registers
    for (int i = 0; i < 31; i++) {
        uart_puts(&console, "x");
        if (i < 10) uart_putc(&console, '0' + i);
        else {
            uart_putc(&console, '0' + (i / 10));
            uart_putc(&console, '0' + (i % 10));
        }
        uart_puts(&console, ": 0x");
        uart_put_hex(&console, regs[i]);
        if (i % 2 == 1) uart_puts(&console, "\r\n");
        else uart_puts(&console, "  ");
    }
    
    // Determine current EL and read appropriate system registers
    u64 current_el, elr, esr, far;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    current_el = (current_el >> 2) & 0x3;
    
    if (current_el == 2) {
        asm volatile("mrs %0, elr_el2" : "=r"(elr));
        asm volatile("mrs %0, esr_el2" : "=r"(esr));
        asm volatile("mrs %0, far_el2" : "=r"(far));
        uart_puts(&console, "\r\n[EL2] ");
    } else {
        asm volatile("mrs %0, elr_el1" : "=r"(elr));
        asm volatile("mrs %0, esr_el1" : "=r"(esr));
        asm volatile("mrs %0, far_el1" : "=r"(far));
        uart_puts(&console, "\r\n[EL1] ");
    }
    
    uart_puts(&console, "ELR: 0x");
    uart_put_hex(&console, elr);
    uart_puts(&console, "\r\nESR: 0x");
    uart_put_hex(&console, esr);
    uart_puts(&console, " (EC=");
    uart_put_hex(&console, (esr >> 26) & 0x3F);
    uart_puts(&console, ")\r\nFAR: 0x");
    uart_put_hex(&console, far);
    uart_puts(&console, "\r\n========================\r\n");
}

void handle_sync_exception(exception_context_t* ctx) {
    extern uart_t console;
    uart_puts(&console, "\r\n[FATAL] Synchronous Exception!\r\n");
    dump_regs(ctx);
    while(1);
}

void handle_irq_exception(exception_context_t* ctx) {
    (void)ctx;
    u32 intid = gicv3_ack_irq();
    if (intid == GMAC_GIC_INTID) {
        gmac_clear_irq();
        gmac_rx_pending = 1;
    }
    // INTID 1023 = spurious interrupt — no EOI needed
    if (intid != 1023u) {
        gicv3_eoi_irq(intid);
    }
}

void handle_fiq_exception(exception_context_t* ctx) {
    extern uart_t console;
    uart_puts(&console, "[FIQ]\r\n");
}

void handle_serror_exception(exception_context_t* ctx) {
    extern uart_t console;
    uart_puts(&console, "[FATAL] SError Exception!\r\n");
    dump_regs(ctx);
    while(1);
}
