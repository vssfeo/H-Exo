// H-Exo Omni-Core: Structured JSON Logger Implementation

#include "logger.h"

static uart_t* log_uart = 0;

// Helper to write string
static inline void write_str(const char* str) {
    if (log_uart) {
        uart_puts(log_uart, str);
    }
}

// Helper to write hex number
static inline void write_hex(u64 val) {
    if (log_uart) {
        uart_put_hex(log_uart, val);
    }
}

// Convert log level to string
static const char* level_to_string(log_level_t level) {
    switch (level) {
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_PERF:  return "PERF";
        case LOG_DEBUG: return "DEBUG";
        default:        return "UNKNOWN";
    }
}

void logger_init(uart_t* uart) {
    log_uart = uart;
}

void log_json(log_level_t level, const char* component, const char* message) {
    if (!log_uart) return;
    
    write_str("{\"level\":\"");
    write_str(level_to_string(level));
    write_str("\",\"component\":\"");
    write_str(component);
    write_str("\",\"message\":\"");
    write_str(message);
    write_str("\"}\r\n");
}

void log_perf(const char* metric, u64 value) {
    if (!log_uart) return;
    
    write_str("{\"level\":\"PERF\",\"metric\":\"");
    write_str(metric);
    write_str("\",\"value\":");
    write_hex(value);
    write_str("}\r\n");
}

void log_event(log_level_t level, const char* component, const char* message, u64 timestamp) {
    if (!log_uart) return;
    
    write_str("{\"level\":\"");
    write_str(level_to_string(level));
    write_str("\",\"component\":\"");
    write_str(component);
    write_str("\",\"message\":\"");
    write_str(message);
    write_str("\",\"timestamp\":");
    write_hex(timestamp);
    write_str("}\r\n");
}
