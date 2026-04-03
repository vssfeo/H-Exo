// H-Exo Omni-Core: L2 Mesh Networking
// Raw Ethernet Layer-2 fabric for zero-copy distributed computing

#ifndef L2_MESH_H
#define L2_MESH_H

#include "../hal/uart.h"

// L2 Frame types (custom EtherType: 0x88B5 - experimental)
#define L2_ETHERTYPE_HEXO   0x88B5

// L2 Mesh frame opcodes
typedef enum {
    L2_OP_HEARTBEAT     = 0x01,  // Node presence announcement
    L2_OP_TASK_MIGRATE  = 0x02,  // Task state migration
    L2_OP_MEM_READ      = 0x03,  // Remote memory read request
    L2_OP_MEM_WRITE     = 0x04,  // Remote memory write
    L2_OP_SYNC_BARRIER  = 0x05,  // Distributed synchronization
    L2_OP_CRYPTO_HELLO  = 0x06,  // Crypto-addressed node discovery
} l2_opcode_t;

// MAC address (6 bytes)
typedef struct {
    u8 addr[6];
} __attribute__((packed)) mac_addr_t;

// L2 Ethernet header
typedef struct {
    mac_addr_t dst;         // Destination MAC
    mac_addr_t src;         // Source MAC
    u16 ethertype;          // 0x88B5 for H-Exo
} __attribute__((packed)) eth_header_t;

// H-Exo L2 Mesh payload header
typedef struct {
    u8 opcode;              // L2_OP_*
    u8 flags;               // Reserved for future use
    u16 payload_len;        // Payload length in bytes
    u32 node_id;            // Sender node ID (crypto hash)
    u64 timestamp;          // Cycle counter timestamp
} __attribute__((packed)) l2_mesh_header_t;

// Complete L2 frame
typedef struct {
    eth_header_t eth;
    l2_mesh_header_t mesh;
    u8 payload[1500 - sizeof(eth_header_t) - sizeof(l2_mesh_header_t)];
} __attribute__((packed)) l2_frame_t;

// L2 Mesh node state
typedef struct {
    mac_addr_t local_mac;
    u32 node_id;
    bool initialized;
    u64 frames_tx;
    u64 frames_rx;
} l2_mesh_node_t;

// API functions
result_t l2_mesh_init(l2_mesh_node_t* node, const mac_addr_t* mac);
result_t l2_mesh_send(l2_mesh_node_t* node, const l2_frame_t* frame);
result_t l2_mesh_recv(l2_mesh_node_t* node, l2_frame_t* frame);
result_t l2_mesh_broadcast_heartbeat(l2_mesh_node_t* node);

#endif // L2_MESH_H
