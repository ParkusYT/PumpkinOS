/* ===========================================================================
 * PumpkinOS - AC'97 audio driver
 * ---------------------------------------------------------------------------
 * Plays raw 8-bit unsigned mono PCM (expanded to 16-bit signed stereo for the
 * codec) via bus-master DMA. Supports two controllers: the Intel ICH AC'97
 * (8086:2415, what QEMU emulates) and the VIA VT82C686 (1106:3058, the chip on
 * the real target board). The AC'97 codec itself is standard across both.
 * ========================================================================= */
#ifndef PUMPKIN_AC97_H
#define PUMPKIN_AC97_H

#include <stdint.h>

/* The /system PCM assets are stored at the codec's fixed rate (no VRA needed,
 * which is unreliable on the VIA VT82C686). */
#define AC97_RATE 48000

/* Fixed high-memory DMA buffer: 48 kHz 16-bit stereo of a few seconds is far
 * too big for the low-memory window below the floppy DMA buffer, so we place
 * it at a fixed physical address and reserve it from the frame allocator. It's
 * in identity-mapped RAM, so its physical address equals its virtual address.
 * It holds two preloaded clips (startup + shutdown) side by side. */
#define AC97_DMA_PHYS  0x00300000u
#define AC97_DMA_BYTES (1024u * 1024u)

/* Preload slots. */
#define AC97_STARTUP   0
#define AC97_SHUTDOWN  1

/* Find the audio controller, enable it and bring up the codec. */
void ac97_init(void);

/* Non-zero if a supported controller was found and initialised. */
int  ac97_present(void);

/* Print controller/codec state to the console (the shell's 'audio' command),
 * for diagnosing why sound isn't playing on real hardware. */
void ac97_debug(void);

/* Read + decode a raw 8-bit unsigned mono PCM file into the given slot's DMA
 * region (AC97_STARTUP / AC97_SHUTDOWN). This is the slow part (floppy I/O),
 * meant to run once at boot so playback later is instant. Returns 0 on success.
 */
int  ac97_preload(int slot, const char *path);

/* Start DMA on a preloaded slot. When 'wait' is non-zero, block until playback
 * finishes; otherwise return immediately (the sound plays in the background). */
void ac97_play(int slot, int wait);

#endif /* PUMPKIN_AC97_H */
