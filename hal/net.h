// H-Exo Omni-Core: L3 Network Stack
// ARP reply + ICMP echo for RK3399/RK3399
// Board IP: 192.168.1.10 (matches U-Boot setenv ipaddr)

#ifndef HAL_NET_H
#define HAL_NET_H

#include "../core/types.h"

// Our static IP on the LAN — must match U-Boot's setenv ipaddr
#define NET_OUR_IP  {192, 168, 1, 10}

// Process one received Ethernet frame.
// Sends ARP reply or ICMP echo reply via gmac_send_raw if applicable.
// Returns OK if a reply was sent, ERR_NOT_FOUND if frame was ignored.
result_t net_process(const u8* frame, usize len);

#endif // HAL_NET_H
