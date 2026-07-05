/* ===========================================================================
 * PumpkinOS - IPv4 core: Ethernet framing, ARP, IPv4, and a generic receive
 * dispatch shared by the UDP, ICMP and TCP layers over the RTL8139.
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

/* one-entry ARP cache (enough to reach the gateway / DNS / a server) */
static uint32_t arp_ip;
static uint8_t  arp_mac[6];
static int      arp_valid;

static uint16_t ip_id = 1;

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

/* Reply to an incoming ICMP echo request (so the box answers pings). */
static void icmp_echo_reply(uint32_t dst, const uint8_t *req, int len) {
    static uint8_t msg[1500];
    if (len > (int)sizeof(msg)) return;
    memcpy(msg, req, (uint32_t)len);
    msg[0] = 0;                                  /* type 0 = echo reply */
    msg[1] = 0;
    msg[2] = msg[3] = 0;                          /* zero csum, recompute */
    uint16_t c = ip_checksum(msg, len);
    msg[2] = (uint8_t)c; msg[3] = (uint8_t)(c >> 8);
    net_send_ip(dst, IP_PROTO_ICMP, msg, len);
}

/* Resolve 'ip' to a MAC, sending ARP requests and polling for the reply. */
static int resolve(uint32_t ip, uint8_t out[6]) {
    if (arp_valid && arp_ip == ip) { memcpy(out, arp_mac, 6); return 1; }

    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    struct net_rx r;
    for (int attempt = 0; attempt < 4; attempt++) {
        arp_valid = 0;
        arp_request(ip);
        uint32_t start = timer_ticks();
        while (timer_ticks() - start < hz / 2) {     /* 500 ms per try */
            net_poll(&r);                            /* handles ARP for us */
            if (arp_valid && arp_ip == ip) { memcpy(out, arp_mac, 6); return 1; }
            io_wait();
        }
    }
    return 0;
}

int net_send_ip(uint32_t dst_ip, uint8_t proto, const void *l4, int l4len) {
    uint8_t dmac[6];
    if (dst_ip == IP_BROADCAST) {
        memcpy(dmac, bcast_mac, 6);
    } else {
        uint32_t nexthop = dst_ip;
        if (net_mask && ((dst_ip & net_mask) != (net_ip & net_mask)))
            nexthop = net_gateway;                   /* off-link -> via gateway */
        if (!resolve(nexthop, dmac))
            return -1;
    }

    static uint8_t pkt[1600];
    int iplen = 20 + l4len;
    if (iplen > 1500)
        return -1;
    struct ip_hdr *ip = (struct ip_hdr *)pkt;
    memset(ip, 0, 20);
    ip->ver_ihl = 0x45;
    ip->len   = htons((uint16_t)iplen);
    ip->id    = htons(ip_id++);
    ip->ttl   = 64;
    ip->proto = proto;
    ip_to_bytes(net_ip, ip->src);
    ip_to_bytes(dst_ip, ip->dst);
    ip->csum  = ip_checksum(ip, 20);
    memcpy(pkt + 20, l4, (uint32_t)l4len);

    return eth_send(dmac, ETH_IPV4, pkt, iplen);
}

/* Poll and process one received frame. Handles ARP and answers pings itself.
 * If an IP datagram for the upper layers (UDP/TCP/ICMP echo reply) arrives,
 * fill *r (pointing into the internal RX buffer) and return 1. */
int net_poll(struct net_rx *r) {
    int n = rtl8139_poll(rxframe, sizeof(rxframe));
    if (n < 14)
        return 0;

    struct eth_hdr *e = (struct eth_hdr *)rxframe;
    uint16_t type = ntohs(e->type);

    if (type == ETH_ARP) {
        struct arp_pkt *a = (struct arp_pkt *)(rxframe + 14);
        uint16_t oper = ntohs(a->oper);
        if (oper == 2) {                             /* reply -> cache it */
            arp_ip = bytes_to_ip(a->spa);
            memcpy(arp_mac, a->sha, 6);
            arp_valid = 1;
        } else if (oper == 1 && net_ip && bytes_to_ip(a->tpa) == net_ip) {
            struct arp_pkt rep;                      /* who-has us -> answer */
            rep.htype = htons(1); rep.ptype = htons(ETH_IPV4);
            rep.hlen = 6; rep.plen = 4; rep.oper = htons(2);
            memcpy(rep.sha, net_mac, 6); ip_to_bytes(net_ip, rep.spa);
            memcpy(rep.tha, a->sha, 6);  memcpy(rep.tpa, a->spa, 4);
            eth_send(a->sha, ETH_ARP, &rep, sizeof(rep));
        }
        return 0;
    }

    if (type != ETH_IPV4)
        return 0;

    struct ip_hdr *ip = (struct ip_hdr *)(rxframe + 14);
    if ((ip->ver_ihl >> 4) != 4)
        return 0;
    int ihl = (ip->ver_ihl & 0x0F) * 4;
    int total = ntohs(ip->len);
    if (ihl < 20 || total < ihl)
        return 0;
    uint8_t *l4 = rxframe + 14 + ihl;
    int l4len = total - ihl;
    if (14 + ihl + l4len > n)                         /* clamp to bytes we got */
        l4len = n - 14 - ihl;
    if (l4len < 0)
        return 0;

    /* Courtesy: answer pings ourselves and don't bubble them up. */
    if (ip->proto == IP_PROTO_ICMP && l4len >= 8 && l4[0] == 8) {
        icmp_echo_reply(bytes_to_ip(ip->src), l4, l4len);
        return 0;
    }

    if (r) {
        r->src_ip = bytes_to_ip(ip->src);
        r->proto  = ip->proto;
        r->l4     = l4;
        r->l4len  = l4len;
    }
    return 1;
}

int net_send_udp(uint32_t dst_ip, uint16_t sport, uint16_t dport,
                 const void *data, int len) {
    static uint8_t seg[1500];
    int udplen = 8 + len;
    if (udplen > (int)sizeof(seg))
        return -1;
    struct udp_hdr *u = (struct udp_hdr *)seg;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len   = htons((uint16_t)udplen);
    u->csum  = 0;                                    /* optional for IPv4 */
    memcpy(seg + 8, data, (uint32_t)len);
    return net_send_ip(dst_ip, IP_PROTO_UDP, seg, udplen);
}

int net_recv_udp(uint16_t my_port, uint32_t *src_ip, uint16_t *sport,
                 void *buf, int maxlen, int timeout_ms) {
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint32_t limit = (uint32_t)timeout_ms * hz / 1000;
    uint32_t start = timer_ticks();
    struct net_rx r;
    while (timer_ticks() - start <= limit) {
        if (net_poll(&r) && r.proto == IP_PROTO_UDP && r.l4len >= 8) {
            struct udp_hdr *u = (struct udp_hdr *)r.l4;
            if (ntohs(u->dport) == my_port) {
                int payload = ntohs(u->len) - 8;
                if (payload < 0) payload = 0;
                if (payload > r.l4len - 8) payload = r.l4len - 8;
                if (payload > maxlen) payload = maxlen;
                memcpy(buf, r.l4 + 8, (uint32_t)payload);
                if (src_ip) *src_ip = r.src_ip;
                if (sport)  *sport  = ntohs(u->sport);
                return payload;
            }
        }
        io_wait();
    }
    return 0;
}
