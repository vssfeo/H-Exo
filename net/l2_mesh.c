// H-Exo Omni-Core: L2 Mesh Networking Implementation
// Raw Ethernet Layer-2 fabric (stub for future GMAC driver integration)

#include "l2_mesh.h"

result_t l2_mesh_init(l2_mesh_node_t* node, const mac_addr_t* mac) {
    if (!node || !mac) return ERR_INVALID_PARAM;
    
    // Copy MAC address
    for (int i = 0; i < 6; i++) {
        node->local_mac.addr[i] = mac->addr[i];
    }
    
    // Generate node ID from MAC (simple hash for now)
    node->node_id = 0;
    for (int i = 0; i < 6; i++) {
        node->node_id ^= (mac->addr[i] << (i * 4));
    }
    
    node->frames_tx = 0;
    node->frames_rx = 0;
    node->initialized = true;
    
    return OK;
}

result_t l2_mesh_send(l2_mesh_node_t* node, const l2_frame_t* frame) {
    if (!node || !frame || !node->initialized) return ERR_INVALID_PARAM;
    
    // TODO: Integrate with RK3399 GMAC driver
    // For now, this is a stub that will be implemented when we add
    // direct register-level Ethernet controller access
    
    node->frames_tx++;
    return ERR_NOT_FOUND; // Not implemented yet
}

result_t l2_mesh_recv(l2_mesh_node_t* node, l2_frame_t* frame) {
    if (!node || !frame || !node->initialized) return ERR_INVALID_PARAM;
    
    // TODO: Integrate with RK3399 GMAC driver
    // Will use DMA ring buffers for zero-copy receive
    
    return ERR_NOT_FOUND; // Not implemented yet
}

result_t l2_mesh_broadcast_heartbeat(l2_mesh_node_t* node) {
    if (!node || !node->initialized) return ERR_INVALID_PARAM;
    
    l2_frame_t frame;
    
    // Set broadcast MAC (FF:FF:FF:FF:FF:FF)
    for (int i = 0; i < 6; i++) {
        frame.eth.dst.addr[i] = 0xFF;
        frame.eth.src.addr[i] = node->local_mac.addr[i];
    }
    
    frame.eth.ethertype = L2_ETHERTYPE_HEXO;
    
    // Set mesh header
    frame.mesh.opcode = L2_OP_HEARTBEAT;
    frame.mesh.flags = 0;
    frame.mesh.payload_len = 0;
    frame.mesh.node_id = node->node_id;
    frame.mesh.timestamp = 0; // TODO: Use PMU cycle counter
    
    return l2_mesh_send(node, &frame);
}
