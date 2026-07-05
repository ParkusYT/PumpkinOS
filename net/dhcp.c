/* ===========================================================================
 * PumpkinOS - DHCP client (DISCOVER -> OFFER -> REQUEST -> ACK)
 * ========================================================================= */
#include "dhcp.h"
#include "net.h"
#include "rtl8139.h"
#include "timer.h"
#include "string.h"
#include <stdint.h>

/* BOOTP/DHCP message: 236-byte fixed part, then options (magic cookie first). */
struct dhcp_msg {
    uint8_t  op, htype, hlen, hops;
    uint8_t  xid[4];
    uint16_t secs, flags;
    uint8_t  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64], file[128];
    uint8_t  options[312];
} __attribute__((packed));

#define DHCP_MSG_LEN 300               /* zero-padded to the BOOTP minimum */

static const uint8_t magic[4] = {0x63, 0x82, 0x53, 0x63};

/* Find option 'want' in the options area; returns its value pointer + length. */
static const uint8_t *find_opt(const uint8_t *opt, int len, uint8_t want, int *vlen) {
    int i = 4;                          /* skip the magic cookie */
    while (i + 1 < len) {
        uint8_t t = opt[i];
        if (t == 0) { i++; continue; }  /* pad  */
        if (t == 255) break;            /* end  */
        uint8_t l = opt[i + 1];
        if (t == want) { *vlen = l; return &opt[i + 2]; }
        i += 2 + l;
    }
    return 0;
}

static void fill_common(struct dhcp_msg *m, const uint8_t xid[4]) {
    memset(m, 0, sizeof(*m));
    m->op = 1; m->htype = 1; m->hlen = 6;
    memcpy(m->xid, xid, 4);
    m->flags = htons(0x8000);           /* ask the server to broadcast the reply */
    memcpy(m->chaddr, net_mac, 6);
    memcpy(m->options, magic, 4);
}

/* Wait for a DHCP reply of the given message type (2=OFFER, 5=ACK) matching
 * xid; returns 1 and leaves it in *out. */
static int wait_reply(const uint8_t xid[4], uint8_t want_type,
                      struct dhcp_msg *out, int timeout_ms) {
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint32_t limit = (uint32_t)timeout_ms * hz / 1000;
    uint32_t start = timer_ticks();
    while (timer_ticks() - start <= limit) {
        int n = net_recv_udp(68, 0, 0, out, sizeof(*out), 500);
        if (n < 240) continue;
        if (out->op != 2 || memcmp(out->xid, xid, 4) != 0) continue;
        int vl;
        const uint8_t *t = find_opt(out->options, n - 236, 53, &vl);
        if (t && vl >= 1 && t[0] == want_type)
            return 1;
    }
    return 0;
}

int dhcp_configure(void) {
    if (!rtl8139_present())
        return 0;

    /* Flush any traffic that piled up (and possibly overflowed the ring) since
     * boot, so we start the exchange with a clean, live receiver. */
    rtl8139_reset_rx();

    /* Reset any prior config so ARP/routing behave during the exchange. */
    net_ip = net_gateway = net_dns = net_mask = 0;
    net_up = 0;

    uint32_t x = timer_ticks() * 2654435761u + 0x9139;
    uint8_t xid[4] = { (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24) };

    static struct dhcp_msg m, reply;

    /* ---- DISCOVER -> OFFER (resend on packet loss / while the link settles) */
    int ok = 0;
    for (int attempt = 0; attempt < 8 && !ok; attempt++) {
        fill_common(&m, xid);
        uint8_t *o = m.options + 4;
        *o++ = 53; *o++ = 1; *o++ = 1;             /* DHCP DISCOVER */
        *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;   /* mask, router, dns */
        *o++ = 255;
        net_send_udp(IP_BROADCAST, 68, 67, &m, DHCP_MSG_LEN);
        ok = wait_reply(xid, 2, &reply, 1000);     /* 1 s per try */
    }
    if (!ok)
        return 0;

    uint32_t offered = bytes_to_ip(reply.yiaddr);
    int vl;
    const uint8_t *sid = find_opt(reply.options, sizeof(reply.options), 54, &vl);
    int have_sid = (sid && vl == 4);
    uint8_t server_id[4] = {0, 0, 0, 0};
    if (have_sid) memcpy(server_id, sid, 4);

    /* ---- REQUEST -> ACK ---- */
    ok = 0;
    for (int attempt = 0; attempt < 8 && !ok; attempt++) {
        fill_common(&m, xid);
        uint8_t *o = m.options + 4;
        *o++ = 53; *o++ = 1; *o++ = 3;             /* DHCP REQUEST */
        *o++ = 50; *o++ = 4; ip_to_bytes(offered, o); o += 4;   /* requested IP */
        if (have_sid) { *o++ = 54; *o++ = 4; memcpy(o, server_id, 4); o += 4; }
        *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
        *o++ = 255;
        net_send_udp(IP_BROADCAST, 68, 67, &m, DHCP_MSG_LEN);
        ok = wait_reply(xid, 5, &reply, 1000);     /* ACK */
    }
    if (!ok)
        return 0;

    /* ---- apply the lease ---- */
    net_ip = bytes_to_ip(reply.yiaddr);
    const uint8_t *v;
    if ((v = find_opt(reply.options, sizeof(reply.options), 1, &vl)) && vl == 4)
        net_mask = bytes_to_ip(v);
    if ((v = find_opt(reply.options, sizeof(reply.options), 3, &vl)) && vl >= 4)
        net_gateway = bytes_to_ip(v);
    if ((v = find_opt(reply.options, sizeof(reply.options), 6, &vl)) && vl >= 4)
        net_dns = bytes_to_ip(v);

    net_up = 1;
    return 1;
}
