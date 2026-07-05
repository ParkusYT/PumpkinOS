/* ===========================================================================
 * PumpkinOS - IPv4/UDP core (Ethernet framing, ARP, IPv4, UDP) over RTL8139
 * ========================================================================= */
#include "net.h"
#include "rtl8139.h"
#include "timer.h"
#include "io.h"
#include "string.h"
#include <stdint.h>

uint8_t  net_mac[6];
uint32_t net_ip, net_gateway, net_dns, net_mask;
int      net_up;

static const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* one-entry ARP cache (enough to reach the gateway / DNS) */
static uint32_t arp_ip;
static uint8_t  arp_mac[6];
static int      arp_valid;

static uint8_t txframe[1600];
static uint8_t rxframe[1600];

int net_init(void) {
    rtl8139_init();
    if (!rtl8139_present())
        return 0;
    memcpy(net_mac, rtl8139_mac(), 6);
    return 1;
}

uint16_t ip_checksum(const void *data, int len) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int eth_send(const uint8_t dst[6], uint16_t ethertype,
                    const void *payload, int plen) {
    struct eth_hdr *e = (struct eth_hdr *)txframe;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, net_mac, 6);
    e->type = htons(ethertype);
    memcpy(txframe + 14, payload, (uint32_t)plen);
    return rtl8139_send(txframe, 14 + plen);
}

static void arp_request(uint32_t tip) {
    struct arp_pkt a;
    a.htype = htons(1); a.ptype = htons(ETH_IPV4);
    a.hlen = 6; a.plen = 4; a.oper = htons(1);
    memcpy(a.sha, net_mac, 6); ip_to_bytes(net_ip, a.spa);
    memset(a.tha, 0, 6);       ip_to_bytes(tip, a.tpa);
    eth_send(bcast_mac, ETH_ARP, &a, sizeof(a));
}

/* Poll and process one received frame. Handles ARP internally. If a UDP
 * datagram to 'want_port' arrives, copy its payload and return the length. */
static int service(uint16_t want_port, uint32_t *src_ip, uint16_t *sport,
                   void *buf, int maxlen) {
    int n = rtl8139_poll(rxframe, sizeof(rxframe));
    if (n < 14)
        return 0;

    struct eth_hdr *e = (struct eth_hdr *)rxframe;
    uint16_t type = ntohs(e->type);

    if (type == ETH_ARP) {
        struct arp_pkt *a = (struct arp_pkt *)(rxframe + 14);
        uint16_t oper = ntohs(a->oper);
        if (oper == 2) {                         /* reply -> cache it */
            arp_ip = bytes_to_ip(a->spa);
            memcpy(arp_mac, a->sha, 6);
            arp_valid = 1;
        } else if (oper == 1 && net_ip && bytes_to_ip(a->tpa) == net_ip) {
            struct arp_pkt r;                    /* who-has us -> answer */
            r.htype = htons(1); r.ptype = htons(ETH_IPV4);
            r.hlen = 6; r.plen = 4; r.oper = htons(2);
            memcpy(r.sha, net_mac, 6); ip_to_bytes(net_ip, r.spa);
            memcpy(r.tha, a->sha, 6);  memcpy(r.tpa, a->spa, 4);
            eth_send(a->sha, ETH_ARP, &r, sizeof(r));
        }
        return 0;
    }

    if (type == ETH_IPV4) {
        struct ip_hdr *ip = (struct ip_hdr *)(rxframe + 14);
        if ((ip->ver_ihl >> 4) != 4 || ip->proto != IP_PROTO_UDP)
            return 0;
        int ihl = (ip->ver_ihl & 0x0F) * 4;
        struct udp_hdr *u = (struct udp_hdr *)(rxframe + 14 + ihl);
        if (ntohs(u->dport) != want_port)
            return 0;
        int payload = ntohs(u->len) - 8;
        if (payload < 0) return 0;
        if (payload > maxlen) payload = maxlen;
        memcpy(buf, (uint8_t *)u + 8, (uint32_t)payload);
        if (src_ip) *src_ip = bytes_to_ip(ip->src);
        if (sport)  *sport  = ntohs(u->sport);
        return payload;
    }
    return 0;
}

/* Resolve 'ip' to a MAC, sending ARP requests and polling for the reply. */
static int resolve(uint32_t ip, uint8_t out[6]) {
    if (arp_valid && arp_ip == ip) { memcpy(out, arp_mac, 6); return 1; }

    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint8_t scratch[4];
    for (int attempt = 0; attempt < 4; attempt++) {
        arp_valid = 0;
        arp_request(ip);
        uint32_t start = timer_ticks();
        while (timer_ticks() - start < hz / 2) {     /* 500 ms per try */
            service(0, 0, 0, scratch, 0);            /* handles ARP */
            if (arp_valid && arp_ip == ip) { memcpy(out, arp_mac, 6); return 1; }
            io_wait();
        }
    }
    return 0;
}

int net_send_udp(uint32_t dst_ip, uint16_t sport, uint16_t dport,
                 const void *data, int len) {
    uint8_t dmac[6];
    if (dst_ip == IP_BROADCAST) {
        memcpy(dmac, bcast_mac, 6);
    } else {
        uint32_t nexthop = dst_ip;
        if (net_mask && ((dst_ip & net_mask) != (net_ip & net_mask)))
            nexthop = net_gateway;               /* off-link -> via gateway */
        if (!resolve(nexthop, dmac))
            return -1;
    }

    static uint8_t pkt[1600];
    struct ip_hdr  *ip = (struct ip_hdr *)pkt;
    struct udp_hdr *u  = (struct udp_hdr *)(pkt + 20);
    int udplen = 8 + len;
    int iplen  = 20 + udplen;

    memset(ip, 0, 20);
    ip->ver_ihl = 0x45;
    ip->len = htons((uint16_t)iplen);
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    ip_to_bytes(net_ip, ip->src);
    ip_to_bytes(dst_ip, ip->dst);
    ip->csum = ip_checksum(ip, 20);

    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len   = htons((uint16_t)udplen);
    u->csum  = 0;                                /* optional for IPv4 */
    memcpy(pkt + 28, data, (uint32_t)len);

    return eth_send(dmac, ETH_IPV4, pkt, iplen);
}

int net_recv_udp(uint16_t my_port, uint32_t *src_ip, uint16_t *sport,
                 void *buf, int maxlen, int timeout_ms) {
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint32_t limit = (uint32_t)timeout_ms * hz / 1000;
    uint32_t start = timer_ticks();
    while (timer_ticks() - start <= limit) {
        int n = service(my_port, src_ip, sport, buf, maxlen);
        if (n > 0) return n;
        io_wait();
    }
    return 0;
}
