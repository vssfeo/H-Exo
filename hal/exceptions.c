#include "../core/types.h"
#include "uart.h"
#include "gicv3.h"
#include "gmac.h"

// Set by IRQ handler; cleared + drained by main loop.
volatile u32 gmac_rx_pending = 0;

// Phase 2: SGI counters per core (cache-line aligned)
volatile u64 __attribute__((aligned(64))) sgi_counters[6][4];  // 6 cores, 4 SGI types

// Total SGI deliveries per core, covering the WHOLE SGI interrupt-ID space the
// handler recognises (0..15), not only the four pipe-it stages.
//
// Why this exists: sgi_counters[] above is indexed by pipe-it stage, so it only
// ever sees intid 0..3. SGIs with intid 4..15 are dispatched by the same
// `if (intid < 16)` block but were counted nowhere, which made "did every core
// receive any SGI" unanswerable from a log. Cache-line aligned and cleaned to
// DRAM by the caller path for the same reason as the other counters: the A72
// cluster does not snoop A53 stores.
volatile u64 __attribute__((aligned(64))) sgi_total[6];

// Phase 2: Pipe-it stage handlers (weak - linked from pipeit.c if present)
__attribute__((weak)) void pipeit_sgi_hidden(u64 arg)  { (void)arg; }
__attribute__((weak)) void pipeit_sgi_output(u64 arg) { (void)arg; }
__attribute__((weak)) void pipeit_sgi_done(u64 arg)   { (void)arg; }
__attribute__((weak)) void ipc_bench_sgi_isr(void)    { }  /* linked from main_neuro.c */

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
    // Deliberate panic halt, not a bug and not an accidental spin. A
    // synchronous exception or an SError on this PE leaves nothing sane to
    // return to: the register frame has already been dumped, and continuing
    // would execute whatever happens to follow a fault we did not expect.
    // The wfi is a real instruction with side effects, which is also what stops
    // -fanalyzer from reporting this loop as an unintentional infinite loop.
    for (;;) { asm volatile("wfi"); }
}

/*
 * H-Exo: witness that the IRQ vector was actually entered, on this PE.
 *
 * The A72 investigation reached a hard boundary: an SGI latches in the A72's own
 * GICR_ISPENDR0 and is never delivered. Everything up to the pending bit is
 * verified correct. These counters are the first witness past the Distributor -
 * they are written from inside the exception handler, BEFORE anything is
 * acknowledged, so a non-zero value proves the vector was reached and a zero
 * value proves the interrupt died somewhere between ISPENDR and the vector.
 *
 * Deliberately independent of gicv3_ack_irq(): if the ack itself faulted we would
 * otherwise never know the handler had run at all.
 */
volatile u64 g_irq_entry_count[6];
volatile u64 g_irq_entry_magic[6];
volatile u64 g_irq_last_intid[6];

void handle_irq_exception(exception_context_t* ctx) {
    (void)ctx;
    u64 mp;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mp));
    u32 a0 = (u32)(mp & 0xFF);
    u32 a1 = (u32)((mp >> 8) & 0xFF);
    u32 c  = a1 ? (a0 + 4u) : a0;
    if (c < 6u) {
        g_irq_entry_count[c]++;
        g_irq_entry_magic[c] = 0x495251454E54524FULL;  /* "IRQENTR" */
        /* Clean to DRAM: the A72 cluster does not snoop A53 stores, so core 0
         * would otherwise read back a stale zero and conclude the vector was
         * never entered. Same defect as the workqueue slot - do not repeat it. */
        asm volatile("dc civac, %0" :: "r"(&g_irq_entry_count[c]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&g_irq_entry_magic[c]) : "memory");
        asm volatile("dsb sy" ::: "memory");
    }
    u32 intid = gicv3_ack_irq();
    if (c < 6u) g_irq_last_intid[c] = (u64)intid;

    // Phase 2: SGI dispatch (INTIDs 0-15)
    if (intid < 16) {
        u64 mpidr;
        asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        u32 aff0 = (u32)(mpidr & 0xFF);
        u32 aff1 = (u32)((mpidr >> 8) & 0xFF);
        u32 core = (aff1 ? (aff0 + 4u) : aff0);
        if (core < 6) {
            /* Count the delivery first, before any stage filtering, so the log
             * can answer "did this core receive an SGI at all". intid 4..15 are
             * real SGIs that sgi_counters[] does not track. */
            sgi_total[core]++;
            if (intid < 4) sgi_counters[core][intid]++;
        }

        switch (intid) {
            case SGI_STAGE_INPUT:
                // Input stage is now inline on Core 0 (submit_frame)
                break;
            case SGI_STAGE_HIDDEN:
                pipeit_sgi_hidden(0);
                break;
            case SGI_STAGE_OUTPUT:
                pipeit_sgi_output(0);
                break;
            case SGI_STAGE_DONE:
                pipeit_sgi_done(0);
                break;
            case 4:   /* ipc bench SGI (generic: only sgi_total[] counts) */
                ipc_bench_sgi_isr();
                break;
            default:
                break;
        }
        gicv3_eoi_irq(intid);
        return;
    }

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
    (void)ctx;
    // Fallback path: some firmware/security configurations can expose SGI as
    // Group0 (FIQ path) instead of Group1 IRQ. Drain IAR0 and dispatch SGIs
    // exactly like IRQ handler.
    u64 iar0;
    asm volatile("mrs %0, S3_0_C12_C8_0" : "=r"(iar0));   // ICC_IAR0_EL1
    u32 intid = (u32)iar0;

    if (intid < 16) {
        u64 mpidr;
        asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        u32 aff0 = (u32)(mpidr & 0xFF);
        u32 aff1 = (u32)((mpidr >> 8) & 0xFF);
        u32 core = (aff1 ? (aff0 + 4u) : aff0);
        if (core < 6) {
            /* Count the delivery first, before any stage filtering, so the log
             * can answer "did this core receive an SGI at all". intid 4..15 are
             * real SGIs that sgi_counters[] does not track. */
            sgi_total[core]++;
            if (intid < 4) sgi_counters[core][intid]++;
        }

        switch (intid) {
            case SGI_STAGE_INPUT:
                break;
            case SGI_STAGE_HIDDEN:
                pipeit_sgi_hidden(0);
                break;
            case SGI_STAGE_OUTPUT:
                pipeit_sgi_output(0);
                break;
            case SGI_STAGE_DONE:
                pipeit_sgi_done(0);
                break;
            case 4:   /* ipc bench SGI (generic: only sgi_total[] counts) */
                ipc_bench_sgi_isr();
                break;
            default:
                break;
        }
    }
    if (intid != 1023u) {
        asm volatile("msr S3_0_C12_C8_1, %0" :: "r"((u64)intid)); // ICC_EOIR0_EL1
        asm volatile("isb");
    }
}

void handle_serror_exception(exception_context_t* ctx) {
    extern uart_t console;
    uart_puts(&console, "[FATAL] SError Exception!\r\n");
    dump_regs(ctx);
    // Deliberate panic halt, not a bug and not an accidental spin. A
    // synchronous exception or an SError on this PE leaves nothing sane to
    // return to: the register frame has already been dumped, and continuing
    // would execute whatever happens to follow a fault we did not expect.
    // The wfi is a real instruction with side effects, which is also what stops
    // -fanalyzer from reporting this loop as an unintentional infinite loop.
    for (;;) { asm volatile("wfi"); }
}
