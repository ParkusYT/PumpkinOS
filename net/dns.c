/* ===========================================================================
 * PumpkinOS - DNS resolver (query one A record over UDP port 53)
 * ========================================================================= */
#include "dns.h"
#include "net.h"
#include "timer.h"
#include "string.h"
#include <stdint.h>

struct dns_hdr {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} __attribute__((packed));

/* Encode "a.b.c" as length-prefixed labels ending in a zero byte. */
static int encode_name(uint8_t *out, const char *name) {
    int len = 0;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int l = (int)(dot - p);
        if (l == 0 || l > 63) return -1;
        out[len++] = (uint8_t)l;
        memcpy(out + len, p, (uint32_t)l);
        len += l;
        p = (*dot == '.') ? dot + 1 : dot;
    }
    out[len++] = 0;
    return len;
}

/* Advance past a (possibly compressed) name, returning the pointer just after
 * it. A compression pointer (0xC0) terminates the name in two bytes. */
static uint8_t *skip_name(uint8_t *p, uint8_t *end) {
    while (p < end) {
        uint8_t c = *p;
        if (c == 0) return p + 1;
        if ((c & 0xC0) == 0xC0) return p + 2;
        p += 1 + c;
    }
    return end;
}

int dns_resolve(const char *hostname, uint32_t *ip_out) {
    if (!net_up || net_dns == 0)
        return 0;

    uint16_t id = (uint16_t)(timer_ticks() * 40503u + 0x1234);
    uint16_t sport = (uint16_t)(0xC000 | (timer_ticks() & 0x0FFF));

    /* ---- build the query ---- */
    static uint8_t q[512];
    struct dns_hdr *h = (struct dns_hdr *)q;
    memset(h, 0, sizeof(*h));
    h->id = htons(id);
    h->flags = htons(0x0100);          /* standard query, recursion desired */
    h->qdcount = htons(1);

    uint8_t *b = q + sizeof(struct dns_hdr);
    int nl = encode_name(b, hostname);
    if (nl < 0) return 0;
    b += nl;
    b[0] = 0; b[1] = 1;                 /* QTYPE  = A  */
    b[2] = 0; b[3] = 1;                 /* QCLASS = IN */
    b += 4;
    int qlen = (int)(b - q);

    if (net_send_udp(net_dns, sport, 53, q, qlen) != 0)
        return 0;

    /* ---- receive + parse the response ---- */
    static uint8_t r[512];
    uint32_t sip; uint16_t sp;
    int n = net_recv_udp(sport, &sip, &sp, r, sizeof(r), 4000);
    if (n < (int)sizeof(struct dns_hdr))
        return 0;

    struct dns_hdr *rh = (struct dns_hdr *)r;
    if (ntohs(rh->id) != id)
        return 0;
    int answers = ntohs(rh->ancount);
    if (answers <= 0)
        return 0;

    uint8_t *end = r + n;
    uint8_t *p = r + sizeof(struct dns_hdr);
    p = skip_name(p, end);             /* question name */
    p += 4;                            /* QTYPE + QCLASS */

    for (int i = 0; i < answers && p + 10 <= end; i++) {
        p = skip_name(p, end);
        if (p + 10 > end) break;
        uint16_t type  = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t rdlen = (uint16_t)((p[8] << 8) | p[9]);
        uint8_t *rdata = p + 10;
        if (type == 1 && rdlen == 4 && rdata + 4 <= end) {   /* A record */
            *ip_out = bytes_to_ip(rdata);
            return 1;
        }
        p = rdata + rdlen;
    }
    return 0;
}
