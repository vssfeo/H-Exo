// H-Exo Omni-Core: Simple Slab Allocator
// Fixed-size block allocator for zero-copy networking and tasks
//
// THREAD SAFETY WARNING:
// This allocator is NOT thread-safe or interrupt-safe in current form.
// For production use with interrupts/multi-core, add:
// 1. Atomic bit operations (LDXR/STXR on AArch64)
// 2. Spinlock protection around bitmap access
// 3. Per-CPU slab caches to reduce contention
//
// FUTURE ENHANCEMENTS:
// - Multiple slab sizes (64B, 256B, 2KB, 4KB)
// - Buddy allocator for large allocations
// - NUMA-aware allocation for multi-socket systems

#include "../hal/uart.h"
#include "slab.h"

#define SLAB_BLOCK_SIZE 2048  // 2KB per block (fits Ethernet frame + metadata)
#define SLAB_MAX_BLOCKS 256   // 512KB total heap

static u8 slab_heap[SLAB_BLOCK_SIZE * SLAB_MAX_BLOCKS] __attribute__((aligned(4096)));
static u8 slab_bitmap[SLAB_MAX_BLOCKS / 8];

static inline bool is_bit_set(int bit) {
    return (slab_bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

static inline void set_bit(int bit) {
    slab_bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void clear_bit(int bit) {
    slab_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static u32 slab_count_used_blocks(void) {
    u32 used = 0;
    for (u32 i = 0; i < SLAB_MAX_BLOCKS; i++) {
        if (is_bit_set((int)i)) {
            used++;
        }
    }
    return used;
}

void slab_init(void) {
    for (int i = 0; i < sizeof(slab_bitmap); i++) {
        slab_bitmap[i] = 0;
    }
}

void* kmalloc(usize size) {
    if (size > SLAB_BLOCK_SIZE) return 0;

    for (int i = 0; i < SLAB_MAX_BLOCKS; i++) {
        if (!is_bit_set(i)) {
            set_bit(i);
            return &slab_heap[i * SLAB_BLOCK_SIZE];
        }
    }
    return 0;
}

void kfree(void* ptr) {
    if (!ptr) return;
    
    usize offset = (u8*)ptr - slab_heap;
    int index = offset / SLAB_BLOCK_SIZE;
    
    if (index >= 0 && index < SLAB_MAX_BLOCKS) {
        clear_bit(index);
    }
}

void slab_dump_stats(uart_t* uart) {
    u32 used = slab_count_used_blocks();
    uart_puts(uart, "[SLAB] Usage: ");
    uart_put_hex(uart, used);
    uart_puts(uart, "/");
    uart_put_hex(uart, SLAB_MAX_BLOCKS);
    uart_puts(uart, " blocks\r\n");
}

u32 slab_get_used_blocks(void) {
    return slab_count_used_blocks();
}

u32 slab_get_total_blocks(void) {
    return SLAB_MAX_BLOCKS;
}

u32 slab_get_usage_percent(void) {
    return (slab_count_used_blocks() * 100u) / SLAB_MAX_BLOCKS;
}
