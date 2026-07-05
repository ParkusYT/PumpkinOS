/* ===========================================================================
 * PumpkinOS - Realtek RTL8139 driver (polled)
 * ---------------------------------------------------------------------------
 * Classic RTL8139 bring-up: find it on PCI, enable bus mastering, reset it,
 * point it at an 8 KiB receive ring, and enable RX/TX. Receiving is done by
 * polling the "RX buffer empty" bit and walking the ring; transmitting cycles
 * through the four hardware TX descriptors. DMA buffers are plain kernel .bss
 * arrays - the kernel is identity-mapped in low memory, so their virtual
 * address equals the physical address the card needs.
 * ========================================================================= */
#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include <stdint.h>

/* Register offsets from the I/O base. */
#define REG_IDR0     0x00
#define REG_TSD0     0x10    /* transmit status descriptor 0..3 (stride 4)  */
#define REG_TSAD0    0x20    /* transmit start address 0..3   (stride 4)    */
#define REG_RBSTART  0x30    /* receive buffer start (physical)             */
#define REG_CMD      0x37
#define REG_CAPR     0x38    /* current address of packet read              */
#define REG_IMR      0x3C
#define REG_ISR      0x3E
#define REG_TCR      0x40
#define REG_RCR      0x44
#define REG_CONFIG1  0x52

#define CMD_RST      0x10
#define CMD_RE       0x08
#define CMD_TE       0x04
#define CMD_BUFE     0x01    /* RX buffer empty */

#define RX_BUF_SIZE  8192
/* 8 KiB ring + 16-byte header slack + a full frame of wrap overrun. */
static uint8_t rx_buffer[RX_BUF_SIZE + 16 + 1536] __attribute__((aligned(4)));
static uint8_t tx_buffer[4][2048] __attribute__((aligned(4)));

static uint16_t io_base;
static uint8_t  mac[6];
static int      present;
static int      tx_cur;
static int      rx_offset;
static uint32_t tx_count, rx_count, tx_err;

static uint32_t phys_of(const void *p) {
    return (uint32_t)(uintptr_t)p;      /* identity-mapped low memory */
}

/* Find the RTL8139 (vendor 0x10EC, device 0x8139) on PCI bus 0. Returns 1 and
 * fills bus/dev/fn if found. */
static int find_card(uint8_t *bus_o, uint8_t *dev_o, uint8_t *fn_o) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read32(0, dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF)
            continue;
        if (id == 0x813910EC) {         /* device<<16 | vendor */
            *bus_o = 0; *dev_o = dev; *fn_o = 0;
            return 1;
        }
    }
    return 0;
}

void rtl8139_init(void) {
    uint8_t bus, dev, fn;
    present = 0;
    if (!find_card(&bus, &dev, &fn))
        return;

    /* Enable I/O space + bus mastering (command register bits 0 and 2). */
    uint32_t cmd = pci_cfg_read32(bus, dev, fn, 0x04);
    pci_cfg_write32(bus, dev, fn, 0x04, cmd | 0x05);

    io_base = (uint16_t)(pci_cfg_read32(bus, dev, fn, 0x10) & 0xFFFC);  /* BAR0 (I/O) */

    outb(io_base + REG_CONFIG1, 0x00);          /* power on */

    outb(io_base + REG_CMD, CMD_RST);           /* software reset */
    for (int i = 0; i < 1000000 && (inb(io_base + REG_CMD) & CMD_RST); i++)
        io_wait();

    outl(io_base + REG_RBSTART, phys_of(rx_buffer));

    /* Read pointer = -16: this tells the card the ring is empty and it should
     * begin writing at offset 0. Real hardware needs it explicitly; QEMU is
     * lenient. */
    outw(io_base + REG_CAPR, 0xFFF0);
    outw(io_base + REG_ISR, 0xFFFF);            /* clear any latched status */
    outw(io_base + REG_IMR, 0x0000);            /* polled: no interrupts */

    /* RCR: accept broadcast + multicast + physical-match + all promiscuous,
     * with ring wrap, unlimited RX DMA burst, and "whole packet" RX FIFO
     * threshold so a frame is only surfaced once fully received. */
    outl(io_base + REG_RCR, 0x0F | (1u << 7) | (7u << 8) | (7u << 13));
    /* TCR: 2048-byte max TX DMA burst, normal interframe gap (avoids the FIFO
     * underruns a zero-config TCR can cause on real silicon). */
    outl(io_base + REG_TCR, 0x03000700u);

    outb(io_base + REG_CMD, CMD_RE | CMD_TE);   /* enable receiver + transmitter */

    for (int i = 0; i < 6; i++)
        mac[i] = inb(io_base + REG_IDR0 + i);

    rx_offset = 0;
    tx_cur = 0;
    present = 1;
}

int rtl8139_present(void)        { return present; }
const uint8_t *rtl8139_mac(void) { return mac; }

int rtl8139_send(const void *frame, int len) {
    if (!present || len <= 0 || len > 1792)
        return -1;

    int n = tx_cur;
    int padded = len < 60 ? 60 : len;          /* min Ethernet frame */
    memcpy(tx_buffer[n], frame, (uint32_t)len);
    if (padded > len)
        memset(tx_buffer[n] + len, 0, (uint32_t)(padded - len));

    outl(io_base + REG_TSAD0 + n * 4, phys_of(tx_buffer[n]));
    /* SIZE (bits 0-12) + early-TX threshold (bits 16-21, in 32-byte units):
     * 0x38 => buffer up to 1792 bytes in the TX FIFO before starting, so the
     * whole frame is staged first and the FIFO can't underrun on real silicon
     * (a zero threshold sends corrupt frames the switch drops). OWN=0 starts. */
    outl(io_base + REG_TSD0 + n * 4, (uint32_t)padded | (0x38u << 16));

    /* Wait for transmit-OK so the frame is really on the wire (and the
     * descriptor is free) before we poll for a reply. */
    int ok = 0;
    for (int i = 0; i < 400000; i++) {
        uint32_t st = inl(io_base + REG_TSD0 + n * 4);
        if (st & 0x8000) { ok = 1; break; }             /* TOK  */
        if (st & ((1u << 30) | (1u << 14))) break;      /* TABT / TUN */
        io_wait();
    }

    tx_count++;
    if (!ok) tx_err++;
    tx_cur = (n + 1) & 3;
    return 0;
}

int rtl8139_poll(void *out, int maxlen) {
    if (!present || (inb(io_base + REG_CMD) & CMD_BUFE))
        return 0;                               /* ring empty */

    uint8_t *p = rx_buffer + rx_offset;
    uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t length = (uint16_t)(p[2] | (p[3] << 8));   /* frame length + 4 (CRC) */
    rx_count++;

    int pktlen = 0;
    if ((status & 0x01) && length >= 4) {       /* ROK, sane length */
        pktlen = length - 4;
        if (pktlen > maxlen)
            pktlen = 0;
        else
            memcpy(out, p + 4, (uint32_t)pktlen);
    }

    rx_offset = (rx_offset + length + 4 + 3) & ~3;   /* skip header, dword-align */
    rx_offset %= RX_BUF_SIZE;
    outw(io_base + REG_CAPR, (uint16_t)(rx_offset - 16));
    outw(io_base + REG_ISR, 0x0001);            /* ack ROK */

    return pktlen;
}

uint32_t rtl8139_tx_count(void) { return tx_count; }
uint32_t rtl8139_rx_count(void) { return rx_count; }
uint32_t rtl8139_tx_err(void)   { return tx_err; }
uint8_t  rtl8139_msr(void)      { return present ? inb(io_base + 0x58) : 0xFF; }
