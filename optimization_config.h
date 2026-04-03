"// H-Exo Optimization Configuration
// Compile-time flags for size/performance optimization

#ifndef HEXO_OPTIMIZATION_CONFIG_H
#define HEXO_OPTIMIZATION_CONFIG_H

// Size Optimization Flags
#ifdef MINIMAL_OUTPUT
    // Maximum size reduction
    #define VERBOSE_OUTPUT        0
    #define DETAILED_LOGGING      0
    #define EXTENDED_TELEMETRY    0
    #define FULL_HEARTBEAT_STATS  0
    #define CHAOS_GENERATOR       0
    #define EXTENDED_UART_BUFFER  0
    #define MULTI_CORE_SUPPORT    0
#else
    // Balanced optimization
    #define VERBOSE_OUTPUT        1
    #define DETAILED_LOGGING      1
    #define EXTENDED_TELEMETRY    1
    #define FULL_HEARTBEAT_STATS  1
    #define CHAOS_GENERATOR       1
    #define EXTENDED_UART_BUFFER  1
    #define MULTI_CORE_SUPPORT    1
#endif

// Neural Network Optimization
#define OPTIMIZED_NEURAL_NET    1
#define REDUCED_NEURAL_LAYERS   1  // 6->4->4 instead of 6->8->4
#define FAST_FIXED_POINT_MATH   1
#define INLINE_ACTIVATION_FUNCS 1
#define REMOVE_WEIGHT_BACKUP    1

// Memory Optimization
#define OPTIMIZED_STACK_SIZE    0x4000  // 16KB instead of 64KB
#define MINIMAL_SLAB_ALLOCATOR  1
#define REDUCED_HEAP_SIZE       1

// Performance Optimization
#define INLINE_CRITICAL_FUNCTIONS 1
#define REDUCED_INTERRUPT_LATENCY 1
#define OPTIMIZED_UART            1
#define MINIMAL_EXCEPTION_HANDLER 1

// Feature Flags
#define ENABLE_TELEMETRY      VERBOSE_OUTPUT
#define ENABLE_LOGGING        DETAILED_LOGGING
#define ENABLE_HEARTBEAT      1
#define ENABLE_NEURAL_SYNC    1
#define ENABLE_MMU            1
#define ENABLE_PMU            1

// Debug Flags (disable for production)
#ifndef NDEBUG
    #define DEBUG_OUTPUT        1
    #define DEBUG_TELEMETRY     1
    #define DEBUG_NEURAL_SYNC   1
#else
    #define DEBUG_OUTPUT        0
    #define DEBUG_TELEMETRY     0
    #define DEBUG_NEURAL_SYNC   0
#endif

#endif // HEXO_OPTIMIZATION_CONFIG_H"