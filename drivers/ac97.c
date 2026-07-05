/* ===========================================================================
 * PumpkinOS - AC'97 audio driver (Intel ICH + VIA VT82C686)
 * ---------------------------------------------------------------------------
 * AC'97 splits into a standard CODEC (volume/rate/DAC) and a vendor-specific
 * bus-master DMA engine. We support two engines:
 *   - Intel ICH  (8086:2415) - two I/O BARs (mixer + bus master), a Buffer
 *     Descriptor List of {addr,len}. This is what QEMU emulates, so it's the
 *     fully-tested path.
 *   - VIA 686    (1106:3058) - one I/O BAR, codec access through a windowed
 *     register at +0x80, and a Scatter-Gather Descriptor table. This is the
 *     chip on the real board.
 * PCM assets are 8-bit unsigned mono; we expand them to 16-bit signed stereo
 * (what the codec wants) into a low-memory DMA buffer, then play one-shot.
 * ========================================================================= */
#include "ac97.h"
#include "pci.h"
#include "io.h"
#include "fat12.h"
#include "timer.h"
#include "string.h"
#include "console.h"
#include <stdint.h>

/* ---- codec (mixer) registers (standard AC'97) ----------------------------- */
#define CODEC_RESET       0x00
#define CODEC_MASTER_VOL  0x02
#define CODEC_PCM_VOL     0x18
#define CODEC_EXT_AUDIO   0x28    /* extended audio ID   (bit0 = VRA support)  */
#define CODEC_EXT_CTRL    0x2A    /* extended audio ctrl (bit0 = VRA enable)   */
#define CODEC_PCM_RATE    0x2C    /* PCM front DAC sample rate                 */
#define CODEC_VENDOR1     0x7C

/* ---- Intel ICH bus-master registers (offset from NABMBAR) ----------------- */
#define ICH_PO_BDBAR  0x10        /* PCM out: buffer descriptor list base      */
#define ICH_PO_LVI    0x15        /* last valid index                          */
#define ICH_PO_SR     0x16        /* status (bit0 = DMA halted)                */
#define ICH_PO_CR     0x1B        /* control (bit0 run, bit1 reset)            */
#define ICH_GLOB_CNT  0x2C        /* global control (bit1 = cold reset#)       */
#define ICH_GLOB_STA  0x30        /* global status  (bit8 = primary ready)     */

/* ---- VIA VT82C686 registers ----------------------------------------------- */
#define VIA_SGD_STATUS  0x00      /* byte: bit7 active, bit2 stopped, bit1 EOL */
#define VIA_SGD_CONTROL 0x01      /* byte: bit7 start, bit6 terminate          */
#define VIA_SGD_TYPE    0x02      /* byte: bit5 16bit, bit4 stereo, ints       */
#define VIA_SGD_TABLE   0x04      /* dword: physical base of the SGD table     */
#define VIA_REG_AC97    0x80      /* dword: windowed codec access              */
#define VIA_AC97_BUSY   (1u << 24)
#define VIA_AC97_READ   (1u << 23)
#define VIA_AC97_PVALID (1u << 25)
/* AC-link control/status live in PCI config space (bytes 0x40/0x41). */
#define VIA_ACLINK_STAT   0x40    /* bit0 = primary codec ready                */
#define VIA_ACLINK_C00_RDY 0x01
#define VIA_ACLINK_CTRL   0x41
#define VIA_ACLINK_ENABLE 0x80
#define VIA_ACLINK_RESET  0x40    /* 1 = de-assert (release) the AC-link reset */
#define VIA_ACLINK_SYNC   0x20
#define VIA_ACLINK_SGD    0x04
#define VIA_ACLINK_INIT   (VIA_ACLINK_ENABLE | VIA_ACLINK_RESET | VIA_ACLINK_SGD) /* 0xC4 */

enum { AC_NONE, AC_ICH, AC_VIA };

static int      variant;
static uint8_t  bus, dev, fn;
static uint16_t io_mixer;         /* ICH NAMBAR                                */
static uint16_t io_bm;            /* ICH NABMBAR / VIA single base             */
static int      present;

/* DMA buffer: 8-bit mono file is read into the top quarter and expanded (x4)
 * forward into 16-bit signed stereo from the start - the write pointer never
 * catches the read pointer while the source stays within the top quarter. */
#define AUDIO_BUF (240 * 1024)
static uint8_t  audio_dma[AUDIO_BUF] __attribute__((aligned(4)));

/* descriptor tables (kept in identity-mapped low memory, so addr == phys) */
static uint32_t ich_bdl[32 * 2] __attribute__((aligned(8)));   /* 32 * {addr,len} */
static uint32_t via_sgd[2 * 2]  __attribute__((aligned(8)));   /* {addr, cnt|flags} */

static uint32_t phys_of(const void *p) { return (uint32_t)(uintptr_t)p; }

static void delay_ms(uint32_t ms) {
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint32_t ticks = ms * hz / 1000; if (!ticks) ticks = 1;
    uint32_t start = timer_ticks();
    while (timer_ticks() - start < ticks) io_wait();
}

/* byte-wide PCI config access (built on the dword primitive) */
static uint8_t cfg_read8(uint8_t off) {
    return (uint8_t)(pci_cfg_read32(bus, dev, fn, off & 0xFC) >> ((off & 3) * 8));
}
static void cfg_write8(uint8_t off, uint8_t val) {
    int sh = (off & 3) * 8;
    uint32_t v = pci_cfg_read32(bus, dev, fn, off & 0xFC);
    v = (v & ~(0xFFu << sh)) | ((uint32_t)val << sh);
    pci_cfg_write32(bus, dev, fn, off & 0xFC, v);
}

/* Codec (mixer) access. The VIA window can take a few microseconds; bound the
 * spin tightly (~4 ms) so a non-responding codec can never hang the boot. */
#define VIA_SPIN 4000

static void codec_write(uint8_t reg, uint16_t val) {
    if (variant == AC_ICH) {
        outw(io_mixer + reg, val);
        return;
    }
    for (int i = 0; i < VIA_SPIN && (inl(io_bm + VIA_REG_AC97) & VIA_AC97_BUSY); i++)
        io_wait();
    outl(io_bm + VIA_REG_AC97, ((uint32_t)reg << 16) | val);
    for (int i = 0; i < VIA_SPIN && (inl(io_bm + VIA_REG_AC97) & VIA_AC97_BUSY); i++)
        io_wait();
}

static uint16_t codec_read(uint8_t reg) {
    if (variant == AC_ICH)
        return inw(io_mixer + reg);
    outl(io_bm + VIA_REG_AC97, VIA_AC97_READ | ((uint32_t)reg << 16));
    /* The window pipelines: right after issuing, it still shows the PREVIOUS
     * register's result. Wait until it reports OUR register (bits 16-22 echo
     * the command) with the valid bit set and busy clear. Fall back to the
     * last valid readback if the echo never matches, so detection can't
     * regress to "not answering". */
    uint32_t last = 0xFFFF;
    for (int i = 0; i < VIA_SPIN; i++) {
        uint32_t v = inl(io_bm + VIA_REG_AC97);
        if (!(v & VIA_AC97_BUSY) && (v & VIA_AC97_PVALID)) {
            last = v;
            if (((v >> 16) & 0x7F) == reg)
                return (uint16_t)v;
        }
        io_wait();
    }
    return (uint16_t)last;
}


/* ---- PCI discovery -------------------------------------------------------- */
static int find_controller(void) {
    for (uint8_t d = 0; d < 32; d++) {
        uint32_t id0 = pci_cfg_read32(0, d, 0, 0x00);
        if ((id0 & 0xFFFF) == 0xFFFF) continue;
        uint8_t htype = (uint8_t)pci_cfg_read16(0, d, 0, 0x0E);
        int nfn = (htype & 0x80) ? 8 : 1;
        for (int f = 0; f < nfn; f++) {
            uint32_t id = pci_cfg_read32(0, d, (uint8_t)f, 0x00);
            uint16_t ven = id & 0xFFFF, dv = id >> 16;
            if (ven == 0xFFFF) continue;
            if (ven == 0x8086 && dv == 0x2415) variant = AC_ICH;   /* ICH AC97 */
            else if (ven == 0x1106 && dv == 0x3058) variant = AC_VIA; /* VT82C686 */
            else continue;
            bus = 0; dev = d; fn = (uint8_t)f;
            return 1;
        }
    }
    return 0;
}

/* ---- init ----------------------------------------------------------------- */
void ac97_init(void) {
    present = 0;
    variant = AC_NONE;
    if (!find_controller())
        return;

    /* enable I/O space + bus mastering */
    uint32_t cmd = pci_cfg_read32(bus, dev, fn, 0x04);
    pci_cfg_write32(bus, dev, fn, 0x04, cmd | 0x05);

    if (variant == AC_ICH) {
        io_mixer = (uint16_t)(pci_cfg_read32(bus, dev, fn, 0x10) & 0xFFFC);  /* BAR0 */
        io_bm    = (uint16_t)(pci_cfg_read32(bus, dev, fn, 0x14) & 0xFFFC);  /* BAR1 */
        outl(io_bm + ICH_GLOB_CNT, 0x02);          /* deassert AC'97 cold reset */
        for (int i = 0; i < 1000; i++) {           /* wait for primary codec    */
            if (inl(io_bm + ICH_GLOB_STA) & 0x100) break;
            delay_ms(1);
        }
        outb(io_bm + ICH_PO_CR, 0x02);             /* reset the PCM-out engine  */
        for (int i = 0; i < 1000 && (inb(io_bm + ICH_PO_CR) & 0x02); i++)
            delay_ms(1);
    } else {                                       /* VIA VT82C686 */
        io_bm = (uint16_t)(pci_cfg_read32(bus, dev, fn, 0x10) & 0xFFFC);     /* BAR0 */
        /* Only do the disruptive AC-link reset if the codec isn't already up
         * (bounded so it can never hang the boot). */
        if (!(cfg_read8(VIA_ACLINK_STAT) & VIA_ACLINK_C00_RDY)) {
            cfg_write8(VIA_ACLINK_CTRL,
                       VIA_ACLINK_ENABLE | VIA_ACLINK_RESET | VIA_ACLINK_SYNC);
            delay_ms(1);
            cfg_write8(VIA_ACLINK_CTRL, VIA_ACLINK_INIT);
            delay_ms(1);
            for (int i = 0; i < 250; i++) {
                if (cfg_read8(VIA_ACLINK_STAT) & VIA_ACLINK_C00_RDY) break;
                delay_ms(1);
            }
        } else {
            /* Codec already up (BIOS did it) - just make sure the SGD DMA link
             * bit is enabled, without toggling the link/codec. */
            cfg_write8(VIA_ACLINK_CTRL,
                       cfg_read8(VIA_ACLINK_CTRL) | VIA_ACLINK_SGD);
        }
    }

    /* Confirm the codec actually answers (bounded); give up cleanly if not, so
     * boot continues and 'ac97' can be used to diagnose. */
    int ready = 0;
    for (int i = 0; i < 200; i++) {
        if (codec_read(CODEC_VENDOR1) != 0xFFFF) { ready = 1; break; }
        delay_ms(1);
    }
    if (!ready)
        return;                                     /* present stays 0 */

    codec_write(CODEC_RESET, 0);
    delay_ms(2);
    codec_write(CODEC_MASTER_VOL, 0x0000);          /* 0 dB, unmuted */
    codec_write(CODEC_PCM_VOL,    0x0808);          /* PCM out, unmuted */

    if (codec_read(CODEC_EXT_AUDIO) & 1) {          /* variable-rate audio */
        codec_write(CODEC_EXT_CTRL, codec_read(CODEC_EXT_CTRL) | 1);
        codec_write(CODEC_PCM_RATE, AC97_RATE);
    }

    present = 1;
}

int ac97_present(void) { return present; }

void ac97_debug(void) {
    if (variant == AC_NONE) {
        console_write("  no AC'97 controller found on PCI\n");
        return;
    }
    console_write("  controller : ");
    console_write(variant == AC_ICH ? "Intel ICH" : "VIA VT82C686");
    console_write(present ? "  (codec ready)\n" : "  (codec NOT ready)\n");

    console_write("  io base    : "); console_write_hex(io_bm);
    if (variant == AC_ICH) { console_write("  mixer "); console_write_hex(io_mixer); }
    console_putc('\n');

    if (variant == AC_VIA) {
        console_write("  ACLINK STAT: "); console_write_hex(cfg_read8(VIA_ACLINK_STAT));
        console_write("  (bit0 = primary codec ready)\n");
        console_write("  ACLINK CTRL: "); console_write_hex(cfg_read8(VIA_ACLINK_CTRL));
        console_write("  (want E/N of 0xC4)\n");
        console_write("  AC97 window: "); console_write_hex(inl(io_bm + VIA_REG_AC97));
        console_putc('\n');
    } else {
        console_write("  GLOB_STA   : "); console_write_hex(inl(io_bm + ICH_GLOB_STA));
        console_write("  (bit8 = primary codec ready)\n");
    }

    console_write("  codec vendr: ");
    console_write_hex(codec_read(0x7C)); console_putc(' ');
    console_write_hex(codec_read(0x7E));
    console_write("  (0xFFFF = codec not answering)\n");
    console_write("  master(02) : "); console_write_hex(codec_read(CODEC_MASTER_VOL));
    console_write("  extaud(28) : "); console_write_hex(codec_read(CODEC_EXT_AUDIO));
    console_write("  rate(2C)   : "); console_write_hex(codec_read(CODEC_PCM_RATE));
    console_putc('\n');
}

/* ---- playback ------------------------------------------------------------- */
static void ich_start(uint32_t buf_phys, int bytes) {
    /* split into <=0xFFFE-sample descriptors (each sample is one 16-bit unit) */
    int idx = 0, off = 0;
    while (off < bytes && idx < 32) {
        int chunk = bytes - off;
        if (chunk > 0xFFFE * 2) chunk = 0xFFFE * 2;
        ich_bdl[idx * 2]     = buf_phys + (uint32_t)off;
        ich_bdl[idx * 2 + 1] = (uint32_t)(chunk / 2);   /* length in samples */
        off += chunk;
        idx++;
    }
    ich_bdl[(idx - 1) * 2 + 1] |= (1u << 31);           /* IOC on last */

    outb(io_bm + ICH_PO_CR, 0x02);                      /* reset engine */
    for (int i = 0; i < 1000 && (inb(io_bm + ICH_PO_CR) & 0x02); i++) io_wait();
    outl(io_bm + ICH_PO_BDBAR, phys_of(ich_bdl));
    outb(io_bm + ICH_PO_LVI, (uint8_t)(idx - 1));
    outw(io_bm + ICH_PO_SR, 0x1C);                      /* clear status bits */
    outb(io_bm + ICH_PO_CR, 0x01);                      /* run */
}

static int ich_done(void) { return inw(io_bm + ICH_PO_SR) & 0x01; }  /* DCH */
static void ich_stop(void) { outb(io_bm + ICH_PO_CR, 0x00); }

static void via_start(uint32_t buf_phys, int bytes) {
    via_sgd[0] = buf_phys;
    via_sgd[1] = (uint32_t)bytes | (1u << 31) | (1u << 30);   /* EOL | FLAG */

    outb(io_bm + VIA_SGD_CONTROL, 0x40);                /* terminate any current */
    io_wait();
    outb(io_bm + VIA_SGD_TYPE, 0x20 | 0x10 | 0x02);     /* 16-bit, stereo, EOL int */
    outl(io_bm + VIA_SGD_TABLE, phys_of(via_sgd));
    outb(io_bm + VIA_SGD_STATUS, 0x03);                 /* clear EOL/FLAG */
    outb(io_bm + VIA_SGD_CONTROL, 0x80);                /* start */
}

static int via_done(void) {
    uint8_t s = inb(io_bm + VIA_SGD_STATUS);
    return (s & 0x02) || !(s & 0x80);                   /* EOL, or not active */
}
static void via_stop(void) { outb(io_bm + VIA_SGD_CONTROL, 0x40); }

int ac97_play_file(const char *path, int wait) {
    if (!present)
        return -1;

    int maxsrc = AUDIO_BUF / 4;
    uint8_t *src = audio_dma + (AUDIO_BUF - maxsrc);    /* top quarter */
    int n = fs_read_path(path, src, (uint32_t)maxsrc);
    if (n <= 0)
        return -1;

    /* 8-bit unsigned mono -> 16-bit signed stereo, expanding forward in place */
    int16_t *dst = (int16_t *)audio_dma;
    for (int i = 0; i < n; i++) {
        int16_t s = (int16_t)(((int)src[i] - 128) << 8);
        dst[2 * i] = s;
        dst[2 * i + 1] = s;
    }
    int bytes = n * 4;

    if (variant == AC_ICH) ich_start(phys_of(audio_dma), bytes);
    else                   via_start(phys_of(audio_dma), bytes);

    if (!wait)
        return 0;

    /* poll until the engine halts, with a generous timeout (duration + 1 s) */
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint32_t limit = ((uint32_t)n / AC97_RATE + 2) * hz;
    uint32_t start = timer_ticks();
    while (timer_ticks() - start < limit) {
        if (variant == AC_ICH ? ich_done() : via_done()) break;
        io_wait();
    }
    if (variant == AC_ICH) ich_stop(); else via_stop();
    return 0;
}
