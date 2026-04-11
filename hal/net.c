// H-Exo Omni-Core: L3 Network Stack
// ARP reply + ICMP echo — no dynamic allocation, static TX buffers only.

#include "net.h"
#include "gmac.h"

static const u8 MY_IP[4] = NET_OUR_IP;

// ---- helpers ---------------------------------------------------------------

static inline u16 get_be16(const u8* p) {
    return ((u16)p[0] << 8) | p[1];
}
static inline void put_be16(u8* p, u16 v) {
    p[0] = (u8)(v >> 8); p[1] = (u8)v;
}
static void net_copy(u8* dst, const u8* src, usize n) {
    for (usize i = 0; i < n; i++) dst[i] = src[i];
}
static int net_eq(const u8* a, const u8* b, usize n) {
    for (usize i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// RFC 1071 Internet checksum over arbitrary byte buffer.
// Call with checksum field zeroed; store result big-endian in packet.
static u16 inet_cksum(const u8* data, usize len) {
    u32 sum = 0;
    for (usize i = 0; i + 1 < len; i += 2)
        sum += ((u32)data[i] << 8) | data[i + 1];
    if (len & 1)
        sum += (u32)data[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(u16)sum;
}

// ---- ARP -------------------------------------------------------------------
// Responds only to ARP requests (oper=1) whose TPA matches our IP.

static result_t handle_arp(const u8* frame, usize len) {
    if (len < 42) return ERR_NOT_FOUND;  // 14 eth + 28 arp

    const u8* a = frame + 14;
    if (get_be16(a + 0) != 0x0001) return ERR_NOT_FOUND;  // HTYPE Ethernet
    if (get_be16(a + 2) != 0x0800) return ERR_NOT_FOUND;  // PTYPE IPv4
    if (a[4] != 6 || a[5] != 4)   return ERR_NOT_FOUND;  // HLEN/PLEN
    if (get_be16(a + 6) != 0x0001) return ERR_NOT_FOUND;  // OPER request
    if (!net_eq(a + 24, MY_IP, 4)) return ERR_NOT_FOUND;  // TPA = us

    const u8* sha = a + 8;   // sender MAC
    const u8* spa = a + 14;  // sender IP
    const u8* my_mac = gmac_get_mac();

    static u8 reply[42];
    // Ethernet
    net_copy(reply + 0, sha,    6);      // dst = requester MAC
    net_copy(reply + 6, my_mac, 6);      // src = our MAC
    put_be16(reply + 12, 0x0806);        // ARP
    // ARP payload
    put_be16(reply + 14, 0x0001);        // HTYPE
    put_be16(reply + 16, 0x0800);        // PTYPE
    reply[18] = 6; reply[19] = 4;        // HLEN PLEN
    put_be16(reply + 20, 0x0002);        // OPER reply
    net_copy(reply + 22, my_mac, 6);     // SHA = our MAC
    net_copy(reply + 28, MY_IP,  4);     // SPA = our IP
    net_copy(reply + 32, sha,    6);     // THA = requester MAC
    net_copy(reply + 36, spa,    4);     // TPA = requester IP

    return gmac_send_raw(reply, 42);
}

// ---- ICMP echo -------------------------------------------------------------
// Responds only to ICMP echo requests (type=8) addressed to our IP.
// Copies payload verbatim, flips type to 0, recomputes both checksums.

static result_t handle_icmp(const u8* frame, usize len) {
    if (len < 42) return ERR_NOT_FOUND;

    const u8* ip = frame + 14;
    usize ip_hlen = (usize)(ip[0] & 0x0F) * 4;
    if (ip_hlen < 20) return ERR_NOT_FOUND;
    if (ip[9] != 0x01) return ERR_NOT_FOUND;              // protocol ICMP
    if (!net_eq(ip + 16, MY_IP, 4)) return ERR_NOT_FOUND; // dst = us

    usize ip_total = (usize)get_be16(ip + 2);
    if (ip_total < ip_hlen + 8) return ERR_NOT_FOUND;
    usize icmp_len  = ip_total - ip_hlen;
    usize reply_len = 14 + ip_hlen + icmp_len;
    if (reply_len > 1520) return ERR_NOT_FOUND;

    const u8* icmp = ip + ip_hlen;
    if (icmp[0] != 8 || icmp[1] != 0) return ERR_NOT_FOUND; // type=echo req

    static u8 reply[1520];

    // Ethernet: swap src/dst MAC
    net_copy(reply + 0, frame + 6, 6);      // dst = sender MAC
    net_copy(reply + 6, gmac_get_mac(), 6); // src = our MAC
    put_be16(reply + 12, 0x0800);           // IPv4

    // IPv4 header: copy, then fix src/dst/ttl/checksum
    net_copy(reply + 14, ip, ip_hlen);
    net_copy(reply + 14 + 12, ip + 16, 4); // src IP = our IP (original dst)
    net_copy(reply + 14 + 16, ip + 12, 4); // dst IP = sender (original src)
    reply[14 + 8] = 64;                     // TTL = 64
    reply[14 + 10] = 0; reply[14 + 11] = 0;
    u16 ip_csum = inet_cksum(reply + 14, ip_hlen);
    put_be16(reply + 14 + 10, ip_csum);

    // ICMP: copy payload, flip type to 0, recompute checksum
    net_copy(reply + 14 + ip_hlen, icmp, icmp_len);
    reply[14 + ip_hlen + 0] = 0;           // type = echo reply
    reply[14 + ip_hlen + 1] = 0;           // code = 0
    reply[14 + ip_hlen + 2] = 0;
    reply[14 + ip_hlen + 3] = 0;
    u16 icmp_csum = inet_cksum(reply + 14 + ip_hlen, icmp_len);
    put_be16(reply + 14 + ip_hlen + 2, icmp_csum);

    return gmac_send_raw(reply, reply_len);
}

// ---- public ----------------------------------------------------------------

result_t net_process(const u8* frame, usize len) {
    if (len < 14) return ERR_NOT_FOUND;
    switch (get_be16(frame + 12)) {
        case 0x0806: return handle_arp(frame, len);
        case 0x0800: return handle_icmp(frame, len);
        default:     return ERR_NOT_FOUND;
    }
}
