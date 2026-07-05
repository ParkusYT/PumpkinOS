/* ===========================================================================
 * PumpkinOS - Realtek RTL8139 10/100 Ethernet driver
 * ---------------------------------------------------------------------------
 * A polled (no-IRQ) driver for the RTL8139, the classic cheap PCI NIC. It
 * finds the card on the PCI bus, brings it up with an 8 KiB receive ring and
 * four transmit descriptors, and exposes raw send/receive of Ethernet frames.
 * ========================================================================= */
#ifndef PUMPKIN_RTL8139_H
#define PUMPKIN_RTL8139_H

#include <stdint.h>

/* Find and initialise the card. Safe to call once at boot. */
void rtl8139_init(void);

/* Non-zero if a card was found and brought up. */
int rtl8139_present(void);

/* The card's 6-byte MAC address (valid after init). */
const uint8_t *rtl8139_mac(void);

/* Transmit one Ethernet frame (raw, including the 14-byte header). Returns 0
 * on success. Frames shorter than the 60-byte minimum are zero-padded. */
int rtl8139_send(const void *frame, int len);

/* Poll for one received frame. Copies up to 'maxlen' bytes into 'buf' and
 * returns the frame length, or 0 if nothing is waiting. */
int rtl8139_poll(void *buf, int maxlen);

/* Restart the receive ring (clears an overflow/stall). */
void rtl8139_reset_rx(void);

/* Non-zero if the link is up. */
int rtl8139_link_up(void);

#endif /* PUMPKIN_RTL8139_H */
