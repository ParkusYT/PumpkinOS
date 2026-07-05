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

/* Tab completion: fill 'out' with the longest common completion of entries in
 * the current directory whose name starts with 'prefix'. Returns the number of
 * matches (out is the single name when 1, the common prefix when more). */
int fs_complete(const char *prefix, char *out, uint32_t outsize);

/* One directory entry, for programmatic listing (e.g. the desktop). */
struct fs_dirent {
    char     name[13];
    int      is_dir;
    uint32_t size;
    uint16_t cluster;               /* first cluster (to descend into a dir) */
};

/* The root directory's cluster handle (pass to fs_readdir). */
uint16_t fs_root_cluster(void);

/* Fill 'out' with up to 'max' entries from the directory whose first cluster is
 * 'cluster' (use fs_root_cluster() for "/"). Returns the number of entries
 * written (hides '.', '..', volume and LFN records). */
int  fs_readdir(uint16_t cluster, struct fs_dirent *out, int max);

/* Directory listing and navigation. */
void fs_list(void);                     /* ls  */
int  fs_cd(const char *name);           /* cd; 0 ok, -1 error */
void fs_pwd(void);                      /* pwd */

/* Reading and writing. */
int  fs_cat(const char *name);          /* cat; -1 if not found */
/* Read a whole file into 'buf' (up to 'maxlen' bytes). Returns the number of
 * bytes read, or -1 if the file does not exist / is a directory / won't fit. */
int  fs_read(const char *name, uint8_t *buf, uint32_t maxlen);
/* Like fs_read but accepts a path with directories ("/system/FOO.PCM"). The
 * current working directory is left unchanged. */
int  fs_read_path(const char *path, uint8_t *buf, uint32_t maxlen);
int  fs_create(const char *name, const char *data, uint32_t len);  /* write/touch */
int  fs_mkdir(const char *name);        /* mkdir */
int  fs_remove(const char *name);       /* rm (files only) */
int  fs_rmdir(const char *name);        /* rmdir (empty directories) */
int  fs_rmrf(const char *name);         /* rm -r (file or directory tree) */

#endif /* PUMPKIN_FAT12_H */
