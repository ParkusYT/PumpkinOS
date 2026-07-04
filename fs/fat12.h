/* ===========================================================================
 * PumpkinOS - FAT12 filesystem (read + write, with directories)
 * ========================================================================= */
#ifndef PUMPKIN_FAT12_H
#define PUMPKIN_FAT12_H

#include <stdint.h>

/* Mount the floppy's FAT12 volume. Returns the number of entries in the root
 * directory, or -1 if no valid FAT12 image is present. */
int  fs_init(void);
int  fs_mounted(void);

/* The current working directory path (for the prompt / 'pwd'). */
const char *fs_cwd(void);

/* Directory listing and navigation. */
void fs_list(void);                     /* ls  */
int  fs_cd(const char *name);           /* cd; 0 ok, -1 error */
void fs_pwd(void);                      /* pwd */

/* Reading and writing. */
int  fs_cat(const char *name);          /* cat; -1 if not found */
int  fs_create(const char *name, const char *data, uint32_t len);  /* write/touch */
int  fs_mkdir(const char *name);        /* mkdir */
int  fs_remove(const char *name);       /* rm (files only) */

#endif /* PUMPKIN_FAT12_H */
