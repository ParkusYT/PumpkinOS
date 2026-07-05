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
 * in identity-mapped RAM, so its physical address equals its virtual address. */
#define AC97_DMA_PHYS  0x00300000u
#define AC97_DMA_BYTES (768u * 1024u)

/* Find the audio controller, enable it and bring up the codec. */
void ac97_init(void);

/* Non-zero if a supported controller was found and initialised. */
int  ac97_present(void);

/* Print controller/codec state to the console (the shell's 'audio' command),
 * for diagnosing why sound isn't playing on real hardware. */
void ac97_debug(void);

/* Play a raw 8-bit unsigned mono PCM file (at AC97_RATE) off the disk. When
 * 'wait' is non-zero, block until playback finishes; otherwise start the DMA
 * and return immediately (the sound plays in the background). Returns 0 on
 * success, -1 on error. */
int  ac97_play_file(const char *path, int wait);

/* Split version of the above so the slow floppy read can be done ahead of time
 * (e.g. preload at boot) and playback started instantly later:
 *   ac97_prepare()       - read + decode the file into the DMA buffer.
 *   ac97_start_prepared() - kick off DMA on whatever was last prepared. */
int  ac97_prepare(const char *path);
void ac97_start_prepared(void);

#endif /* PUMPKIN_AC97_H */
