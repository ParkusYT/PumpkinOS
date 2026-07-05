/* ===========================================================================
 * PumpkinOS - ICMP echo (ping) client
 * ========================================================================= */
#include "icmp.h"
#include "net.h"
#include "timer.h"
#include "string.h"
#include <stdint.h>

#define PING_ID   0x504B          /* 'PK' */
#define PING_DATA 32

static uint16_t ping_seq = 0;

int icmp_ping(uint32_t ip, int timeout_ms, uint32_t *rtt_ms) {
    uint8_t msg[sizeof(struct icmp_hdr) + PING_DATA];
    struct icmp_hdr *h = (struct icmp_hdr *)msg;
    uint16_t seq = ++ping_seq;

    h->type = 8;                                     /* echo request */
    h->code = 0;
    h->csum = 0;
    h->id   = htons(PING_ID);
    h->seq  = htons(seq);
    for (int i = 0; i < PING_DATA; i++)              /* filler payload */
        msg[sizeof(*h) + i] = (uint8_t)('a' + (i & 15));
    h->csum = ip_checksum(msg, (int)sizeof(msg));

    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint32_t start = timer_ticks();

    if (net_send_ip(ip, IP_PROTO_ICMP, msg, (int)sizeof(msg)) != 0)
        return 0;

    uint32_t limit = (uint32_t)timeout_ms * hz / 1000;
    struct net_rx r;
    while (timer_ticks() - start <= limit) {
        if (net_poll(&r) && r.proto == IP_PROTO_ICMP && r.src_ip == ip &&
            r.l4len >= (int)sizeof(struct icmp_hdr)) {
            struct icmp_hdr *rh = (struct icmp_hdr *)r.l4;
            if (rh->type == 0 && ntohs(rh->id) == PING_ID &&
                ntohs(rh->seq) == seq) {
                if (rtt_ms) {
                    uint32_t dt = timer_ticks() - start;
                    *rtt_ms = dt * 1000 / hz;
                }
                return 1;
            }
        }
    }
    return 0;
}
