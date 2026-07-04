/* ===========================================================================
 * PumpkinOS - FAT12 filesystem (read-only, over the boot RAM disk)
 * ---------------------------------------------------------------------------
 * The bootloader copies a small FAT12 image off the floppy into memory at
 * 0x30000. This driver parses its BPB and walks the FAT / root directory to
 * list files and stream their contents - no disk driver needed.
 * ========================================================================= */
#ifndef PUMPKIN_FAT12_H
#define PUMPKIN_FAT12_H

#include <stdint.h>

/* Mount the RAM-disk filesystem. Returns the number of files in the root
 * directory, or -1 if no valid FAT12 image is present. */
int fs_init(void);

/* Non-zero once a filesystem is mounted. */
int fs_mounted(void);

/* List the root directory (the shell's 'ls'). */
void fs_list(void);

/* Print a file's contents (the shell's 'cat'). Returns 0 on success,
 * -1 if the file was not found. Name matching is case-insensitive. */
int fs_cat(const char *name);

#endif /* PUMPKIN_FAT12_H */
