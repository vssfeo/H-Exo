// H-Exo Omni-Core: Core Type Definitions
// Zero-dependency type system for bare metal

#ifndef HEXO_CORE_TYPES_H
#define HEXO_CORE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Core types
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;

typedef uintptr_t uptr;
typedef size_t    usize;

// Physical and Virtual addresses
typedef u64 paddr_t;    // Physical address
typedef u64 vaddr_t;    // Virtual address

// Node identity for distributed mesh (512-bit)
typedef struct {
    u64 hash[8];        // Hardware-backed public key hash
} node_id_t;

// Memory region descriptor
typedef struct {
    vaddr_t base;
    usize   size;
    u32     attributes;
    u32     flags;
} mem_region_t;

// Result type for error handling
typedef enum {
    OK = 0,
    ERR_INVALID_PARAM,
    ERR_OUT_OF_MEMORY,
    ERR_NOT_FOUND,
    ERR_PERMISSION_DENIED,
    ERR_TIMEOUT,
    ERR_HARDWARE_FAULT
} result_t;

// CPU core information
typedef struct {
    u8  core_id;
    u8  cluster_id;
    u16 reserved;
    u32 features;       // CPU feature flags
} core_info_t;

#endif // HEXO_CORE_TYPES_H
