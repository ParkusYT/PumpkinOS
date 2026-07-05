/* ===========================================================================
 * PumpkinOS - minimal TCP client
 * ---------------------------------------------------------------------------
 * Enough TCP to open a connection, send a request and read the response back:
 * a 3-way handshake, stop-and-wait transmit, in-order receive with ACKs, and a
 * graceful close. Synchronous and polled - no timers, windows or reordering.
 * ========================================================================= */
#ifndef PUMPKIN_TCP_H
#define PUMPKIN_TCP_H

#include <stdint.h>

struct tcp_conn {
    uint32_t dst_ip;
    uint16_t dst_port, sport;
    uint32_t snd_nxt;      /* next sequence number we will send   */
    uint32_t rcv_nxt;      /* next sequence number we expect      */
    int      established;
    int      peer_fin;     /* the peer has sent us a FIN (EOF)    */
};

/* Open a connection to ip:port. Returns 1 on success (handshake complete). */
int  tcp_connect(struct tcp_conn *c, uint32_t ip, uint16_t port);

/* Send 'len' bytes, waiting for each segment to be acknowledged. Returns the
 * number of bytes sent, or -1 on failure. */
int  tcp_send(struct tcp_conn *c, const void *data, int len);

/* Receive up to 'maxlen' bytes (provide at least a full segment, ~1460).
 * Returns >0 bytes read, 0 on peer close (FIN), or <0 on error/timeout. */
int  tcp_recv(struct tcp_conn *c, void *buf, int maxlen, int timeout_ms);

/* Close the connection (send FIN, drain the peer's FIN best-effort). */
void tcp_close(struct tcp_conn *c);

#endif /* PUMPKIN_TCP_H */
