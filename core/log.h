// H-Exo Omni-Core: Logging macros
// Compile with -DDEBUG to enable LOG_DBG output.
// All macros expand to a single uart_puts call on the global `console`.

#ifndef CORE_LOG_H
#define CORE_LOG_H

#include "../hal/uart.h"

extern uart_t console;

#define LOG_OK(msg)    uart_puts(&console, "[OK] "   msg "\r\n")
#define LOG_WARN(msg)  uart_puts(&console, "[WARN] " msg "\r\n")
#define LOG_ERR(msg)   uart_puts(&console, "[ERR] "  msg "\r\n")
#define LOG_INFO(msg)  uart_puts(&console, "[*] "    msg "\r\n")

#ifdef DEBUG
#define LOG_DBG(msg)   uart_puts(&console, "[DBG] "  msg "\r\n")
#else
#define LOG_DBG(msg)   ((void)0)
#endif

#endif // CORE_LOG_H
