/* ===========================================================================
 * PumpkinOS - minimal TCP client
 * ========================================================================= */
#include "tcp.h"
#include "net.h"
#include "timer.h"
#include "io.h"
#include "string.h"
#include <stdint.h>

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* signed sequence comparison (handles wraparound): a >= b ? */
static int seq_geq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

/* TCP checksum over the IPv4 pseudo-header + the segment. */
static uint16_t tcp_checksum(uint32_t dst, const uint8_t *seg, int seglen) {
    static uint8_t buf[12 + 1500];
    if (seglen > 1500) return 0;
    ip_to_bytes(net_ip, buf);
    ip_to_bytes(dst, buf + 4);
    buf[8]  = 0;
    buf[9]  = IP_PROTO_TCP;
    buf[10] = (uint8_t)(seglen >> 8);
    buf[11] = (uint8_t)seglen;
    memcpy(buf + 12, seg, (uint32_t)seglen);
    return ip_checksum(buf, 12 + seglen);
}

/* Build and send one segment (header, at c->snd_nxt / c->rcv_nxt, + data). */
static int tcp_xmit(struct tcp_conn *c, uint8_t flags, const void *data, int dlen) {
    static uint8_t seg[1500];
    if (dlen < 0 || 20 + dlen > (int)sizeof(seg))
        return -1;
    struct tcp_hdr *t = (struct tcp_hdr *)seg;
    t->sport  = htons(c->sport);
    t->dport  = htons(c->dst_port);
    t->seq    = htonl(c->snd_nxt);
    t->ack    = htonl(c->rcv_nxt);
    t->off    = 0x50;                    /* 20-byte header, no options */
    t->flags  = flags;
    t->window = htons(8192);
    t->csum   = 0;
    t->urg    = 0;
    if (dlen)
        memcpy(seg + 20, data, (uint32_t)dlen);
    t->csum = tcp_checksum(c->dst_ip, seg, 20 + dlen);
    return net_send_ip(c->dst_ip, IP_PROTO_TCP, seg, 20 + dlen);
}

/* Pull the next TCP segment addressed to this connection out of the wire, up
 * to 'deadline' ticks. Returns 1 and fills the out params, or 0 on timeout. */
static int tcp_next(struct tcp_conn *c, uint32_t deadline,
                    uint32_t *seq, uint32_t *ack, uint8_t *flags,
                    uint8_t **data, int *dlen) {
    struct net_rx r;
    while ((int32_t)(deadline - timer_ticks()) >= 0) {
        if (net_poll(&r) && r.proto == IP_PROTO_TCP &&
            r.src_ip == c->dst_ip && r.l4len >= 20) {
            struct tcp_hdr *t = (struct tcp_hdr *)r.l4;
            if (ntohs(t->sport) != c->dst_port || ntohs(t->dport) != c->sport) {
                io_wait();
                continue;
            }
            int thl = (t->off >> 4) * 4;
            if (thl < 20 || thl > r.l4len) { io_wait(); continue; }
            *seq   = ntohl(t->seq);
            *ack   = ntohl(t->ack);
            *flags = t->flags;
            *data  = r.l4 + thl;
            *dlen  = r.l4len - thl;
            return 1;
        }
        io_wait();
    }
    return 0;
}

int tcp_connect(struct tcp_conn *c, uint32_t ip, uint16_t port) {
    memset(c, 0, sizeof(*c));
    c->dst_ip   = ip;
    c->dst_port = port;
    c->sport    = (uint16_t)(40000 + (timer_ticks() & 0x3FFF));
    uint32_t isn = timer_ticks() * 2654435761u + 0xC0DE1234u;

    uint32_t hz = timer_hz(); if (!hz) hz = 100;

    for (int attempt = 0; attempt < 5; attempt++) {
        c->snd_nxt = isn;
        c->rcv_nxt = 0;
        tcp_xmit(c, TCP_SYN, 0, 0);          /* SYN */

        uint32_t deadline = timer_ticks() + hz;   /* 1 s */
        uint32_t seq, ack; uint8_t fl; uint8_t *d; int dl;
        while (tcp_next(c, deadline, &seq, &ack, &fl, &d, &dl)) {
            if (fl & TCP_RST)
                return 0;
            if ((fl & TCP_SYN) && (fl & TCP_ACK) && ack == isn + 1) {
                c->rcv_nxt = seq + 1;
                c->snd_nxt = isn + 1;
                tcp_xmit(c, TCP_ACK, 0, 0);   /* complete the handshake */
                c->established = 1;
                return 1;
            }
        }
    }
    return 0;
}

int tcp_send(struct tcp_conn *c, const void *data, int len) {
    if (!c->established || len < 0)
        return -1;
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    const uint8_t *p = (const uint8_t *)data;
    int sent = 0;

    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 512) chunk = 512;         /* conservative segment size */
        uint32_t seg_seq = c->snd_nxt;
        int acked = 0;

        for (int attempt = 0; attempt < 6 && !acked; attempt++) {
            c->snd_nxt = seg_seq;
            tcp_xmit(c, TCP_PSH | TCP_ACK, p + sent, chunk);

            uint32_t deadline = timer_ticks() + hz;   /* 1 s */
            uint32_t seq, ack; uint8_t fl; uint8_t *d; int dl;
            while (tcp_next(c, deadline, &seq, &ack, &fl, &d, &dl)) {
                if (fl & TCP_RST) return -1;
                if ((fl & TCP_ACK) && seq_geq(ack, seg_seq + chunk)) {
                    acked = 1;
                    break;
                }
            }
        }
        if (!acked)
            return -1;
        c->snd_nxt = seg_seq + chunk;
        sent += chunk;
    }
    return sent;
}

int tcp_recv(struct tcp_conn *c, void *buf, int maxlen, int timeout_ms) {
    if (c->peer_fin)
        return 0;
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint32_t deadline = timer_ticks() + (uint32_t)timeout_ms * hz / 1000;

    uint32_t seq, ack; uint8_t fl; uint8_t *d; int dl;
    while (tcp_next(c, deadline, &seq, &ack, &fl, &d, &dl)) {
        if (fl & TCP_RST) { c->peer_fin = 1; return -1; }

        if (seq == c->rcv_nxt) {                  /* in-order segment */
            int n = 0;
            if (dl > 0) {
                n = dl > maxlen ? maxlen : dl;
                memcpy(buf, d, (uint32_t)n);
                c->rcv_nxt += (uint32_t)dl;      /* consume the whole segment */
            }
            if (fl & TCP_FIN) { c->rcv_nxt += 1; c->peer_fin = 1; }
            tcp_xmit(c, TCP_ACK, 0, 0);          /* acknowledge progress */
            if (n > 0)        return n;
            if (c->peer_fin)  return 0;          /* EOF */
            /* a pure ACK / keep-alive: keep waiting for data */
        } else {
            tcp_xmit(c, TCP_ACK, 0, 0);          /* re-ACK; drop dup/reorder */
        }
    }
    return -1;                                    /* timeout */
}

void tcp_close(struct tcp_conn *c) {
    if (!c->established)
        return;
    tcp_xmit(c, TCP_FIN | TCP_ACK, 0, 0);
    c->snd_nxt += 1;                              /* FIN consumes a sequence */

    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint32_t deadline = timer_ticks() + hz;       /* drain ~1 s, best-effort */
    uint32_t seq, ack; uint8_t fl; uint8_t *d; int dl;
    while (tcp_next(c, deadline, &seq, &ack, &fl, &d, &dl)) {
        if (fl & TCP_RST) break;
        if (seq == c->rcv_nxt) {
            if (dl > 0) c->rcv_nxt += (uint32_t)dl;
            if (fl & TCP_FIN) { c->rcv_nxt += 1; c->peer_fin = 1; }
            tcp_xmit(c, TCP_ACK, 0, 0);
        }
        if (c->peer_fin && seq_geq(ack, c->snd_nxt))
            break;
    }
    c->established = 0;
}
