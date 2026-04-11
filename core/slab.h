#ifndef HEXO_CORE_SLAB_H
#define HEXO_CORE_SLAB_H

#include "types.h"
#include "../hal/uart.h"

void slab_init(void);
void* kmalloc(usize size);
void kfree(void* ptr);
void slab_dump_stats(uart_t* uart);
u32 slab_get_used_blocks(void);
u32 slab_get_total_blocks(void);
u32 slab_get_usage_percent(void);

#endif // HEXO_CORE_SLAB_H
