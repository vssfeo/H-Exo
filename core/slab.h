#ifndef HEXO_CORE_SLAB_H
#define HEXO_CORE_SLAB_H

#include "types.h"
#include "../hal/uart.h"

void slab_init(void);
void* kmalloc(usize size);
void kfree(void* ptr);
void slab_dump_stats(uart_t* uart);

#endif // HEXO_CORE_SLAB_H
