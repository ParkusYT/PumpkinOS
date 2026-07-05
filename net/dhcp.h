/* ===========================================================================
 * PumpkinOS - DHCP client
 * ========================================================================= */
#ifndef PUMPKIN_DHCP_H
#define PUMPKIN_DHCP_H

/* Run a full DISCOVER/OFFER/REQUEST/ACK exchange. On success fills net_ip,
 * net_mask, net_gateway, net_dns and sets net_up. Returns 1 on success. */
int dhcp_configure(void);

#endif /* PUMPKIN_DHCP_H */
