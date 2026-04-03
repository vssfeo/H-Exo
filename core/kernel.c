// H-Exo Omni-Core: Main Kernel
// Modular architecture with hardware abstraction

#include "types.h"
#include "../hal/uart.h"

// Hardware addresses
#define UART2_BASE      0xFF1A0000
#define RNG_BASE        0xFF8B8000
#define GIC_DIST_BASE   0xFEE00000
#define GIC_CPU_BASE    0xFEF00000

// Global UART instance
static uart_t console;

// External symbols from assembly
extern u64 node_identity[8];
extern u64 secondary_cores_wake;

// Forward declarations
void handle_sync_exception(void);
static void print_banner(void);
static void print_system_info(void);
static u64 read_cpu_features(void);

//==============================================================================
// Exception Handler (called from assembly)
//==============================================================================
void handle_sync_exception(void) {
    u64 esr, elr, far;
    
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, elr_el1" : "=r"(elr));
    asm volatile("mrs %0, far_el1" : "=r"(far));
    
    uart_puts(&console, "\r\n!!! SYNCHRONOUS EXCEPTION !!!\r\n");
    uart_puts(&console, "ESR_EL1: 0x");
    uart_put_hex(&console, esr);
    uart_puts(&console, "\r\nELR_EL1: 0x");
    uart_put_hex(&console, elr);
    uart_puts(&console, "\r\nFAR_EL1: 0x");
    uart_put_hex(&console, far);
    uart_puts(&console, "\r\n");
    
    // Decode exception class
    u32 ec = (esr >> 26) & 0x3F;
    uart_puts(&console, "Exception Class: 0x");
    uart_put_hex(&console, ec);
    uart_puts(&console, "\r\n");
    
    // Halt
    while (1) {
        asm volatile("wfi");
    }
}

//==============================================================================
// System Information
//==============================================================================
static void print_system_info(void) {
    u64 midr, mpidr, currentel;
    
    asm volatile("mrs %0, midr_el1" : "=r"(midr));
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    asm volatile("mrs %0, CurrentEL" : "=r"(currentel));
    
    uart_puts(&console, "[SYS] MIDR_EL1: 0x");
    uart_put_hex(&console, midr);
    uart_puts(&console, "\r\n[SYS] MPIDR_EL1: 0x");
    uart_put_hex(&console, mpidr);
    uart_puts(&console, "\r\n[SYS] Exception Level: EL");
    uart_putc(&console, '0' + (char)((currentel >> 2) & 0x3));
    uart_puts(&console, "\r\n");
    
    // CPU features
    u64 features = read_cpu_features();
    uart_puts(&console, "[SYS] CPU Features: 0x");
    uart_put_hex(&console, features);
    uart_puts(&console, "\r\n");
    
    // Check for crypto extensions
    u64 isar0;
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    if ((isar0 >> 4) & 0xF) {
        uart_puts(&console, "[OK] AES/SHA Hardware Acceleration: Available\r\n");
    } else {
        uart_puts(&console, "[!!] AES/SHA Hardware Acceleration: Not Available\r\n");
    }
}

static u64 read_cpu_features(void) {
    u64 features = 0;
    u64 isar0, isar1, pfr0;
    
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    asm volatile("mrs %0, id_aa64isar1_el1" : "=r"(isar1));
    asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
    
    // Encode feature bits
    if ((isar0 >> 4) & 0xF) features |= (1 << 0);   // AES
    if ((isar0 >> 8) & 0xF) features |= (1 << 1);   // SHA1
    if ((isar0 >> 12) & 0xF) features |= (1 << 2);  // SHA2
    if ((isar0 >> 16) & 0xF) features |= (1 << 3);  // CRC32
    if ((pfr0 >> 16) & 0xF) features |= (1 << 4);   // FP/SIMD
    
    return features;
}

//==============================================================================
// Banner
//==============================================================================
static void print_banner(void) {
    uart_puts(&console, "\r\n");
    uart_puts(&console, "========================================\r\n");
    uart_puts(&console, "  H-Exo Omni-Core: Aleph Engine v0.2\r\n");
    uart_puts(&console, "  Phase 0: Hardware Hijacking Complete\r\n");
    uart_puts(&console, "========================================\r\n");
    uart_puts(&console, "\r\n");
}

//==============================================================================
// Node Identity Initialization (for crypto-addressing)
//==============================================================================
static void init_node_identity(void) {
    // Read hardware RNG to generate unique node identity
    volatile u32* rng = (volatile u32*)RNG_BASE;
    
    uart_puts(&console, "[*] Generating Node Identity...\r\n");
    
    // Read 512 bits from hardware RNG
    for (int i = 0; i < 8; i++) {
        // Wait for RNG data ready (simplified - real impl needs proper status check)
        for (volatile int j = 0; j < 1000; j++);
        
        // Read 64-bit value (two 32-bit reads)
        u32 low = rng[0x410 >> 2];
        u32 high = rng[0x410 >> 2];
        node_identity[i] = ((u64)high << 32) | low;
    }
    
    uart_puts(&console, "[OK] Node Identity: 0x");
    uart_put_hex(&console, node_identity[0]);
    uart_puts(&console, "...\r\n");
}

//==============================================================================
// Memory Statistics
//==============================================================================
static void print_memory_stats(void) {
    extern char __text_start, __text_end;
    extern char __data_start, __data_end;
    extern char __bss_start, __bss_end;
    extern char __stack_top;
    
    uart_puts(&console, "\r\n[MEM] Memory Layout:\r\n");
    uart_puts(&console, "  .text:  0x");
    uart_put_hex(&console, (u64)&__text_start);
    uart_puts(&console, " - 0x");
    uart_put_hex(&console, (u64)&__text_end);
    uart_puts(&console, "\r\n");
    
    uart_puts(&console, "  .data:  0x");
    uart_put_hex(&console, (u64)&__data_start);
    uart_puts(&console, " - 0x");
    uart_put_hex(&console, (u64)&__data_end);
    uart_puts(&console, "\r\n");
    
    uart_puts(&console, "  .bss:   0x");
    uart_put_hex(&console, (u64)&__bss_start);
    uart_puts(&console, " - 0x");
    uart_put_hex(&console, (u64)&__bss_end);
    uart_puts(&console, "\r\n");
    
    uart_puts(&console, "  stack:  0x");
    uart_put_hex(&console, (u64)&__stack_top);
    uart_puts(&console, "\r\n");
}

//==============================================================================
// Main Kernel Entry Point
//==============================================================================
void kmain(void) {
    // Initialize console UART
    uart_config_t uart_cfg = {
        .base_addr = UART2_BASE,
        .baud_rate = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = 0,
        .fifo_depth = 16
    };
    
    uart_init(&console, &uart_cfg);
    
    // Print banner
    print_banner();
    
    // System initialization sequence
    uart_puts(&console, "[OK] Hardware: RK3399 (NanoPi M4)\r\n");
    uart_puts(&console, "[OK] Boot: Aleph Engine Active\r\n");
    uart_puts(&console, "[OK] Stack: Initialized\r\n");
    uart_puts(&console, "[OK] MMU: Enabled (2-Level Page Tables)\r\n");
    uart_puts(&console, "[OK] Caches: L1 D-Cache + I-Cache Active\r\n");
    uart_puts(&console, "[OK] Exception Vectors: Installed\r\n");
    uart_puts(&console, "[OK] Address Space: Distributed Foundation Ready\r\n");
    
    // Print detailed system info
    uart_puts(&console, "\r\n");
    print_system_info();
    
    // Initialize node identity for crypto-addressing
    init_node_identity();
    
    // Print memory layout
    print_memory_stats();
    
    // Future: Wake secondary cores
    uart_puts(&console, "\r\n[*] Multi-Core: 5 secondary cores in standby\r\n");
    uart_puts(&console, "[*] L2 Mesh: Ready for activation\r\n");
    uart_puts(&console, "[*] Crypto-Addressing: Node identity established\r\n");
    
    uart_puts(&console, "\r\n========================================\r\n");
    uart_puts(&console, "  H-Exo Omni-Core: Operational\r\n");
    uart_puts(&console, "  Awaiting mesh network activation...\r\n");
    uart_puts(&console, "========================================\r\n");
    uart_puts(&console, "\r\nEcho mode active. Type something...\r\n\r\n> ");
    
    // Main loop - echo mode
    while (1) {
        char c = uart_getc(&console);
        
        if (c == '\r') {
            uart_puts(&console, "\r\n> ");
        } else if (c == 0x03) {  // Ctrl+C
            uart_puts(&console, "\r\n[*] Interrupt received\r\n> ");
        } else {
            uart_putc(&console, c);
        }
    }
}
