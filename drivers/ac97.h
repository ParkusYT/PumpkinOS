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

/* Sample rate the /system PCM assets are stored at (set on the codec via VRA). */
#define AC97_RATE 11025

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

#endif /* PUMPKIN_AC97_H */
