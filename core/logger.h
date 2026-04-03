// H-Exo Omni-Core: Structured JSON Logger
// Machine-parseable logging for automated testing and monitoring

#ifndef HEXO_CORE_LOGGER_H
#define HEXO_CORE_LOGGER_H

#include "types.h"
#include "../hal/uart.h"

// Log levels
typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_PERF,
    LOG_DEBUG
} log_level_t;

// Initialize logger with UART handle
void logger_init(uart_t* uart);

// Log structured JSON message
// Format: {"level":"INFO","component":"MMU","message":"Enabled"}
void log_json(log_level_t level, const char* component, const char* message);

// Log performance metric
// Format: {"level":"PERF","metric":"boot_time_ms","value":234}
void log_perf(const char* metric, u64 value);

// Log event with timestamp
// Format: {"level":"INFO","component":"BOOT","message":"Started","timestamp":12345678}
void log_event(log_level_t level, const char* component, const char* message, u64 timestamp);

#endif // HEXO_CORE_LOGGER_H
