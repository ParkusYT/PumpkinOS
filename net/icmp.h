/* ===========================================================================
 * PumpkinOS - ICMP echo (ping)
 * ========================================================================= */
#ifndef PUMPKIN_ICMP_H
#define PUMPKIN_ICMP_H

#include <stdint.h>

/* Send one ICMP echo request to 'ip' and wait up to 'timeout_ms' for the
 * reply. Returns 1 on reply (and stores the round-trip time in *rtt_ms if not
 * NULL), 0 on timeout. */
int icmp_ping(uint32_t ip, int timeout_ms, uint32_t *rtt_ms);

#endif /* PUMPKIN_ICMP_H */
