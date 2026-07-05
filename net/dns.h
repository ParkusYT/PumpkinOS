/* ===========================================================================
 * PumpkinOS - DNS resolver (A records over UDP)
 * ========================================================================= */
#ifndef PUMPKIN_DNS_H
#define PUMPKIN_DNS_H

#include <stdint.h>

/* Resolve 'hostname' to an IPv4 address using the configured DNS server
 * (net_dns). Returns 1 and fills *ip_out (host order) on success. */
int dns_resolve(const char *hostname, uint32_t *ip_out);

#endif /* PUMPKIN_DNS_H */
