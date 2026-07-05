/* ===========================================================================
 * PumpkinOS - minimal IPv4/UDP network stack (Ethernet + ARP + IPv4 + UDP)
 * ---------------------------------------------------------------------------
 * Just enough to run a DHCP client and a DNS resolver over the RTL8139. It is
 * synchronous and polled: send a datagram, then poll for the reply. IP
 * addresses are carried as host-order uint32 (a.b.c.d == (a<<24)|...|d).
 * ========================================================================= */
#ifndef PUMPKIN_NET_H
#define PUMPKIN_NET_H

#include <stdint.h>

/* ---- byte order (x86 is little-endian) ------------------------------------ */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8) |
           ((x >> 8) & 0xFF00u) | ((x >> 24) & 0xFFu);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* host-order IP <-> the 4 on-wire bytes */
static inline void ip_to_bytes(uint32_t ip, uint8_t b[4]) {
    b[0] = (uint8_t)(ip >> 24); b[1] = (uint8_t)(ip >> 16);
    b[2] = (uint8_t)(ip >> 8);  b[3] = (uint8_t)ip;
}
static inline uint32_t bytes_to_ip(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

#define IP_BROADCAST 0xFFFFFFFFu

/* ---- on-wire headers ------------------------------------------------------ */
struct eth_hdr { uint8_t dst[6], src[6]; uint16_t type; } __attribute__((packed));

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[6], spa[4], tha[6], tpa[4];
} __attribute__((packed));

struct ip_hdr {
    uint8_t  ver_ihl, tos;
    uint16_t len, id, frag;
    uint8_t  ttl, proto;
    uint16_t csum;
    uint8_t  src[4], dst[4];
} __attribute__((packed));

struct udp_hdr { uint16_t sport, dport, len, csum; } __attribute__((packed));

#define ETH_ARP  0x0806
#define ETH_IPV4 0x0800
#define IP_PROTO_UDP 17

/* ---- configuration (filled in by DHCP) ------------------------------------ */
extern uint8_t  net_mac[6];
extern uint32_t net_ip;         /* 0 until configured */
extern uint32_t net_gateway;
extern uint32_t net_dns;
extern uint32_t net_mask;
extern int      net_up;

/* Bring up the NIC and read its MAC. Non-zero if a card is present. */
int  net_init(void);

uint16_t ip_checksum(const void *data, int len);

/* Send a UDP datagram to dst_ip (use IP_BROADCAST for 255.255.255.255).
 * Resolves the next hop by ARP as needed. Returns 0 on success. */
int net_send_udp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 const void *data, int len);

/* Poll for a UDP datagram addressed to 'my_port', up to 'timeout_ms'. Returns
 * the payload length (0 on timeout) and fills src_ip/src_port + buf. Handles
 * incoming ARP (replies to who-has for us, caches answers) while waiting. */
int net_recv_udp(uint16_t my_port, uint32_t *src_ip, uint16_t *src_port,
                 void *buf, int maxlen, int timeout_ms);

#endif /* PUMPKIN_NET_H */
