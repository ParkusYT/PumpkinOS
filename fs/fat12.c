/* ===========================================================================
 * PumpkinOS - FAT12 filesystem (read + write, with directories)
 * ---------------------------------------------------------------------------
 * The whole floppy is one FAT12 volume, read and written through the floppy
 * controller. The FAT is cached in memory and flushed to both on-disk copies
 * when it changes; directory sectors and file data are read-modify-written on
 * demand. Supports the root directory and subdirectories (create/enter).
 * ========================================================================= */
#include "fat12.h"
#include "floppy.h"
#include "rtc.h"
#include "console.h"
#include "kheap.h"
#include "string.h"
#include <stdint.h>

#define ATTR_LFN    0x0F
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10
#define ATTR_FILE   0x20
#define EOC         0x0FF8u        /* >= this = end of cluster chain */

/* BPB-derived geometry. */
static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint16_t reserved_sectors;
static uint8_t  num_fats;
static uint16_t root_entries;
static uint16_t sectors_per_fat;
static uint16_t total_secs;
static uint32_t fat_start, root_start, root_sectors, data_start;
static uint32_t cluster_count;      /* number of data clusters */

static uint8_t *fat_cache;          /* the whole FAT, kept in sync */
static int      mounted;

static uint16_t cwd_cluster;        /* current directory (0 = root) */
static char     cwd_path[128];

/* ---- low-level sector I/O (volume starts at LBA 0) ------------------------ */
static int read_fs(uint32_t lba, uint32_t count, uint8_t *buf) {
    for (uint32_t i = 0; i < count; i++)
        if (floppy_read_sector(lba + i, buf + i * 512) != 0)
            return -1;
    return 0;
}
static int write_fs(uint32_t lba, uint32_t count, const uint8_t *buf) {
    for (uint32_t i = 0; i < count; i++)
        if (floppy_write_sector(lba + i, buf + i * 512) != 0)
            return -1;
    return 0;
}

/* ---- FAT (cached) --------------------------------------------------------- */
static uint16_t fat_get(uint16_t cluster) {
    uint32_t off = cluster + (cluster / 2);
    uint16_t v = (uint16_t)(fat_cache[off] | (fat_cache[off + 1] << 8));
    return (cluster & 1) ? (v >> 4) : (v & 0x0FFF);
}
static void fat_put(uint16_t cluster, uint16_t value) {
    uint32_t off = cluster + (cluster / 2);
    uint16_t v = (uint16_t)(fat_cache[off] | (fat_cache[off + 1] << 8));
    if (cluster & 1)
        v = (uint16_t)((v & 0x000F) | (value << 4));
    else
        v = (uint16_t)((v & 0xF000) | (value & 0x0FFF));
    fat_cache[off]     = (uint8_t)(v & 0xFF);
    fat_cache[off + 1] = (uint8_t)(v >> 8);
}
static void fat_flush(void) {
    for (uint8_t i = 0; i < num_fats; i++)
        write_fs(reserved_sectors + (uint32_t)i * sectors_per_fat,
                 sectors_per_fat, fat_cache);
}
static uint16_t alloc_cluster(void) {
    for (uint16_t c = 2; c < cluster_count + 2; c++)
        if (fat_get(c) == 0) {
            fat_put(c, 0xFFF);
            return c;
        }
    return 0;                       /* disk full */
}
static void free_chain(uint16_t cluster) {
    while (cluster >= 2 && cluster < EOC) {
        uint16_t next = fat_get(cluster);
        fat_put(cluster, 0);
        cluster = next;
    }
}

/* First data-sector LBA of a cluster. */
static uint32_t cluster_lba(uint16_t cluster) {
    return data_start + (uint32_t)(cluster - 2) * sectors_per_cluster;
}

/* The index-th 512-byte sector of a directory (dir_cluster 0 = root), or 0. */
static uint32_t dir_sector_lba(uint16_t dir_cluster, uint32_t index) {
    if (dir_cluster == 0) {
        if (index >= root_sectors) return 0;
        return root_start + index;
    }
    uint32_t ci = index / sectors_per_cluster;
    uint32_t si = index % sectors_per_cluster;
    uint16_t c = dir_cluster;
    for (uint32_t i = 0; i < ci; i++) {
        c = fat_get(c);
        if (c < 2 || c >= EOC) return 0;
    }
    return cluster_lba(c) + si;
}

/* ---- name conversion ------------------------------------------------------ */
static void to_83(const char *name, uint8_t *out) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0;
    while (*name && *name != '.' && i < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i++] = (uint8_t)c;
    }
    while (*name && *name != '.') name++;
    if (*name == '.') {
        name++;
        int j = 8;
        while (*name && j < 11) {
            char c = *name++;
            if (c >= 'a' && c <= 'z') c -= 32;
            out[j++] = (uint8_t)c;
        }
    }
}
static void format_name(const uint8_t *e, char *out) {
    int n = 0;
    for (int j = 0; j < 8 && e[j] != ' '; j++) out[n++] = (char)e[j];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int j = 8; j < 11 && e[j] != ' '; j++) out[n++] = (char)e[j];
    }
    out[n] = '\0';
}

/* ---- directory operations ------------------------------------------------- */
/* Find entry 'name83' in dir_cluster. Returns 1 and (optionally) the sector
 * LBA, byte offset, and a copy of the 32-byte entry. */
static int dir_find(uint16_t dir_cluster, const uint8_t *name83,
                    uint32_t *out_lba, uint32_t *out_off, uint8_t *out_entry) {
    uint8_t sec[512];
    for (uint32_t idx = 0; ; idx++) {
        uint32_t lba = dir_sector_lba(dir_cluster, idx);
        if (lba == 0) return 0;
        if (read_fs(lba, 1, sec) != 0) return 0;
        for (int e = 0; e < 16; e++) {
            uint8_t *ent = sec + e * 32;
            if (ent[0] == 0x00) return 0;
            if (ent[0] == 0xE5 || ent[11] == ATTR_LFN) continue;
            if (memcmp(ent, name83, 11) == 0) {
                if (out_lba)   *out_lba = lba;
                if (out_off)   *out_off = (uint32_t)(e * 32);
                if (out_entry) memcpy(out_entry, ent, 32);
                return 1;
            }
        }
    }
}

/* Add a 32-byte entry to dir_cluster, growing a subdirectory if needed. */
static int dir_add(uint16_t dir_cluster, const uint8_t *entry) {
    uint8_t sec[512];
    for (uint32_t idx = 0; ; idx++) {
        uint32_t lba = dir_sector_lba(dir_cluster, idx);
        if (lba == 0) {
            if (dir_cluster == 0) return -1;         /* root can't grow */
            uint16_t c = dir_cluster;                /* find last cluster */
            while (fat_get(c) >= 2 && fat_get(c) < EOC) c = fat_get(c);
            uint16_t nc = alloc_cluster();
            if (nc == 0) return -1;
            fat_put(c, nc);
            fat_flush();
            memset(sec, 0, 512);
            for (uint32_t s = 0; s < sectors_per_cluster; s++)
                write_fs(cluster_lba(nc) + s, 1, sec);
            memcpy(sec, entry, 32);
            write_fs(cluster_lba(nc), 1, sec);
            return 0;
        }
        if (read_fs(lba, 1, sec) != 0) return -1;
        for (int e = 0; e < 16; e++) {
            uint8_t *ent = sec + e * 32;
            if (ent[0] == 0x00 || ent[0] == 0xE5) {
                memcpy(ent, entry, 32);
                return write_fs(lba, 1, sec);
            }
        }
    }
}

/* Current time encoded into FAT's packed date and time words. */
static void fat_now(uint16_t *fdate, uint16_t *ftime) {
    struct rtc_time t;
    rtc_read(&t);
    uint16_t y = (t.year >= 1980) ? (uint16_t)(t.year - 1980) : 0;
    *fdate = (uint16_t)((y << 9) | ((t.month & 0x0F) << 5) | (t.day & 0x1F));
    *ftime = (uint16_t)(((t.hour & 0x1F) << 11) | ((t.minute & 0x3F) << 5) |
                        ((t.second / 2) & 0x1F));
}

static void make_entry(const uint8_t *name83, uint8_t attr,
                       uint16_t first_cluster, uint32_t size, uint8_t *out) {
    memset(out, 0, 32);
    memcpy(out, name83, 11);
    out[11] = attr;

    uint16_t fdate, ftime;
    fat_now(&fdate, &ftime);
    out[14] = (uint8_t)(ftime & 0xFF);  out[15] = (uint8_t)(ftime >> 8);  /* create time */
    out[16] = (uint8_t)(fdate & 0xFF);  out[17] = (uint8_t)(fdate >> 8);  /* create date */
    out[18] = (uint8_t)(fdate & 0xFF);  out[19] = (uint8_t)(fdate >> 8);  /* access date */
    out[22] = (uint8_t)(ftime & 0xFF);  out[23] = (uint8_t)(ftime >> 8);  /* write time  */
    out[24] = (uint8_t)(fdate & 0xFF);  out[25] = (uint8_t)(fdate >> 8);  /* write date  */

    out[26] = (uint8_t)(first_cluster & 0xFF);
    out[27] = (uint8_t)(first_cluster >> 8);
    out[28] = (uint8_t)(size & 0xFF);
    out[29] = (uint8_t)(size >> 8);
    out[30] = (uint8_t)(size >> 16);
    out[31] = (uint8_t)(size >> 24);
}

/* ---- mount ---------------------------------------------------------------- */
int fs_mounted(void) { return mounted; }
const char *fs_cwd(void) { return cwd_path; }

int fs_init(void) {
    mounted = 0;

    uint8_t bpb[512];
    if (floppy_read_sector(0, bpb) != 0) { floppy_motor_off(); return -1; }

    bytes_per_sector    = (uint16_t)(bpb[11] | (bpb[12] << 8));
    sectors_per_cluster = bpb[13];
    reserved_sectors    = (uint16_t)(bpb[14] | (bpb[15] << 8));
    num_fats            = bpb[16];
    root_entries        = (uint16_t)(bpb[17] | (bpb[18] << 8));
    total_secs          = (uint16_t)(bpb[19] | (bpb[20] << 8));
    sectors_per_fat     = (uint16_t)(bpb[22] | (bpb[23] << 8));

    if (bytes_per_sector != 512 || sectors_per_cluster == 0 ||
        num_fats == 0 || num_fats > 2) { floppy_motor_off(); return -1; }

    fat_start    = reserved_sectors;
    root_start   = reserved_sectors + (uint32_t)num_fats * sectors_per_fat;
    root_sectors = ((uint32_t)root_entries * 32 + 511) / 512;
    data_start   = root_start + root_sectors;
    cluster_count = (total_secs - data_start) / sectors_per_cluster;

    fat_cache = (uint8_t *)kmalloc((uint32_t)sectors_per_fat * 512);
    if (!fat_cache || read_fs(fat_start, sectors_per_fat, fat_cache) != 0) {
        floppy_motor_off();
        return -1;
    }
    floppy_motor_off();

    mounted = 1;
    cwd_cluster = 0;
    cwd_path[0] = '/';
    cwd_path[1] = '\0';

    int files = 0;
    uint8_t sec[512];
    for (uint32_t idx = 0; ; idx++) {
        uint32_t lba = dir_sector_lba(0, idx);
        if (lba == 0 || read_fs(lba, 1, sec) != 0) break;
        for (int e = 0; e < 16; e++) {
            uint8_t *ent = sec + e * 32;
            if (ent[0] == 0x00) { floppy_motor_off(); return files; }
            if (ent[0] == 0xE5 || ent[11] == ATTR_LFN || (ent[11] & ATTR_VOLUME))
                continue;
            files++;
        }
    }
    floppy_motor_off();
    return files;
}

/* ---- ls ------------------------------------------------------------------- */
void fs_list(void) {
    if (!mounted) { console_write("ls: no filesystem mounted\n"); return; }

    int count = 0;
    uint8_t sec[512];
    for (uint32_t idx = 0; ; idx++) {
        uint32_t lba = dir_sector_lba(cwd_cluster, idx);
        if (lba == 0 || read_fs(lba, 1, sec) != 0) break;
        for (int e = 0; e < 16; e++) {
            uint8_t *ent = sec + e * 32;
            if (ent[0] == 0x00) goto done;
            if (ent[0] == 0xE5 || ent[11] == ATTR_LFN || (ent[11] & ATTR_VOLUME))
                continue;
            if (ent[0] == '.') continue;          /* hide '.' and '..' */

            char name[13];
            format_name(ent, name);
            console_write("  ");
            console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
            int w = 0;
            for (const char *p = name; *p; p++, w++) console_putc(*p);
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            while (w++ < 14) console_putc(' ');
            if (ent[11] & ATTR_DIR) {
                console_write("<DIR>");
            } else {
                uint32_t size = ent[28] | (ent[29] << 8) | (ent[30] << 16) |
                                ((uint32_t)ent[31] << 24);
                console_write_dec(size);
                console_write(" bytes");
            }
            console_putc('\n');
            count++;
        }
    }
done:
    floppy_motor_off();
    console_write("  ");
    console_write_dec((uint32_t)count);
    console_write(count == 1 ? " entry\n" : " entries\n");
}

/* ---- cat ------------------------------------------------------------------ */
int fs_cat(const char *name) {
    if (!mounted) { console_write("cat: no filesystem mounted\n"); return 0; }

    uint8_t name83[11];
    to_83(name, name83);
    uint8_t ent[32];
    if (!dir_find(cwd_cluster, name83, 0, 0, ent)) { floppy_motor_off(); return -1; }
    if (ent[11] & ATTR_DIR) {
        console_write("cat: is a directory\n");
        floppy_motor_off();
        return 0;
    }

    uint16_t cluster = (uint16_t)(ent[26] | (ent[27] << 8));
    uint32_t remaining = ent[28] | (ent[29] << 8) | (ent[30] << 16) |
                         ((uint32_t)ent[31] << 24);
    uint32_t cbytes = (uint32_t)sectors_per_cluster * 512;
    uint8_t sec[512];
    int guard = 0;
    while (cluster >= 2 && cluster < EOC && remaining > 0 && guard++ < 8192) {
        for (uint32_t s = 0; s < sectors_per_cluster && remaining > 0; s++) {
            if (read_fs(cluster_lba(cluster) + s, 1, sec) != 0) goto done;
            uint32_t n = remaining < 512 ? remaining : 512;
            for (uint32_t k = 0; k < n; k++) console_putc((char)sec[k]);
            remaining -= n;
        }
        (void)cbytes;
        cluster = fat_get(cluster);
    }
done:
    floppy_motor_off();
    return 0;
}

/* ---- create / touch ------------------------------------------------------- */
int fs_create(const char *name, const char *data, uint32_t len) {
    if (!mounted) { console_write("no filesystem mounted\n"); return -1; }

    uint8_t name83[11];
    to_83(name, name83);

    uint32_t lba = 0, off = 0;
    uint8_t existing[32];
    int exists = dir_find(cwd_cluster, name83, &lba, &off, existing);
    if (exists) {
        if (existing[11] & ATTR_DIR) {
            console_write("create: is a directory\n");
            floppy_motor_off();
            return -1;
        }
        free_chain((uint16_t)(existing[26] | (existing[27] << 8)));
    }

    uint32_t cbytes = (uint32_t)sectors_per_cluster * 512;
    uint16_t first = 0, prev = 0;
    uint32_t written = 0;
    while (written < len) {
        uint16_t c = alloc_cluster();
        if (c == 0) { free_chain(first); fat_flush(); floppy_motor_off();
                      console_write("create: disk full\n"); return -1; }
        if (first == 0) first = c;
        if (prev) fat_put(prev, c);
        prev = c;

        uint8_t sec[512];
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            memset(sec, 0, 512);
            uint32_t n = len - written;
            if (n > 512) n = 512;
            if (n) memcpy(sec, data + written, n);
            write_fs(cluster_lba(c) + s, 1, sec);
            written += n;
        }
        (void)cbytes;
    }
    fat_flush();

    uint8_t entry[32];
    make_entry(name83, ATTR_FILE, first, len, entry);
    if (exists) {
        uint8_t sec[512];
        read_fs(lba, 1, sec);
        memcpy(sec + off, entry, 32);
        write_fs(lba, 1, sec);
    } else if (dir_add(cwd_cluster, entry) != 0) {
        free_chain(first); fat_flush(); floppy_motor_off();
        console_write("create: directory full\n");
        return -1;
    }
    floppy_motor_off();
    return 0;
}

/* ---- mkdir ---------------------------------------------------------------- */
int fs_mkdir(const char *name) {
    if (!mounted) { console_write("no filesystem mounted\n"); return -1; }

    uint8_t name83[11];
    to_83(name, name83);
    if (dir_find(cwd_cluster, name83, 0, 0, 0)) {
        console_write("mkdir: already exists\n");
        floppy_motor_off();
        return -1;
    }

    uint16_t nc = alloc_cluster();
    if (nc == 0) { console_write("mkdir: disk full\n"); floppy_motor_off(); return -1; }

    /* Initialise the new directory cluster with '.' and '..' entries. */
    static const uint8_t dot[11]    = {'.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
    static const uint8_t dotdot[11] = {'.','.',' ',' ',' ',' ',' ',' ',' ',' ',' '};
    uint8_t sec[512];
    memset(sec, 0, 512);
    make_entry(dot,    ATTR_DIR, nc,          0, sec);
    make_entry(dotdot, ATTR_DIR, cwd_cluster, 0, sec + 32);
    write_fs(cluster_lba(nc), 1, sec);
    memset(sec, 0, 512);
    for (uint32_t s = 1; s < sectors_per_cluster; s++)
        write_fs(cluster_lba(nc) + s, 1, sec);
    fat_flush();

    uint8_t entry[32];
    make_entry(name83, ATTR_DIR, nc, 0, entry);
    if (dir_add(cwd_cluster, entry) != 0) {
        free_chain(nc); fat_flush(); floppy_motor_off();
        console_write("mkdir: directory full\n");
        return -1;
    }
    floppy_motor_off();
    return 0;
}

/* ---- rm (files only) ------------------------------------------------------ */
int fs_remove(const char *name) {
    if (!mounted) return -1;

    uint8_t name83[11];
    to_83(name, name83);
    uint32_t lba, off;
    uint8_t ent[32];
    if (!dir_find(cwd_cluster, name83, &lba, &off, ent)) { floppy_motor_off(); return -1; }
    if (ent[11] & ATTR_DIR) {
        console_write("rm: is a directory\n");
        floppy_motor_off();
        return 0;
    }

    free_chain((uint16_t)(ent[26] | (ent[27] << 8)));
    fat_flush();
    uint8_t sec[512];
    read_fs(lba, 1, sec);
    sec[off] = 0xE5;                 /* mark deleted */
    write_fs(lba, 1, sec);
    floppy_motor_off();
    return 0;
}

/* ---- rmdir (empty directories only) --------------------------------------- */
int fs_rmdir(const char *name) {
    if (!mounted) return -1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        console_write("rmdir: refusing to remove '.' or '..'\n");
        return 0;
    }

    uint8_t name83[11];
    to_83(name, name83);
    uint32_t lba, off;
    uint8_t ent[32];
    if (!dir_find(cwd_cluster, name83, &lba, &off, ent)) { floppy_motor_off(); return -1; }
    if (!(ent[11] & ATTR_DIR)) {
        console_write("rmdir: not a directory\n");
        floppy_motor_off();
        return 0;
    }

    uint16_t dclus = (uint16_t)(ent[26] | (ent[27] << 8));

    /* The directory must be empty (only '.' and '..'). */
    uint8_t sec[512];
    for (uint32_t idx = 0; ; idx++) {
        uint32_t l = dir_sector_lba(dclus, idx);
        if (l == 0) break;
        if (read_fs(l, 1, sec) != 0) break;
        for (int e = 0; e < 16; e++) {
            uint8_t *en = sec + e * 32;
            if (en[0] == 0x00) goto empty;
            if (en[0] == 0xE5 || en[11] == ATTR_LFN) continue;
            if (en[0] == '.') continue;              /* '.' and '..' */
            console_write("rmdir: directory not empty\n");
            floppy_motor_off();
            return 0;
        }
    }
empty:
    free_chain(dclus);
    fat_flush();
    read_fs(lba, 1, sec);
    sec[off] = 0xE5;                 /* mark the parent entry deleted */
    write_fs(lba, 1, sec);
    floppy_motor_off();
    return 0;
}

/* ---- rm -r (recursive delete) --------------------------------------------- */
/* Free everything inside a directory (recursing into subdirectories). The
 * caller frees the directory's own cluster chain afterwards. */
static void rmrf_cluster(uint16_t dir_cluster, int depth) {
    if (depth > 32) return;                     /* runaway guard */
    uint8_t sec[512];
    for (uint32_t idx = 0; ; idx++) {
        uint32_t lba = dir_sector_lba(dir_cluster, idx);
        if (lba == 0) return;
        if (read_fs(lba, 1, sec) != 0) return;
        for (int e = 0; e < 16; e++) {
            uint8_t *ent = sec + e * 32;
            if (ent[0] == 0x00) return;
            if (ent[0] == 0xE5 || ent[11] == ATTR_LFN) continue;
            if (ent[0] == '.') continue;        /* '.' and '..' */
            uint16_t c = (uint16_t)(ent[26] | (ent[27] << 8));
            if (ent[11] & ATTR_DIR)
                rmrf_cluster(c, depth + 1);
            free_chain(c);
        }
    }
}

int fs_rmrf(const char *name) {
    if (!mounted) return -1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        console_write("rm: refusing to remove '.' or '..'\n");
        return 0;
    }

    uint8_t name83[11];
    to_83(name, name83);
    uint32_t lba, off;
    uint8_t ent[32];
    if (!dir_find(cwd_cluster, name83, &lba, &off, ent)) { floppy_motor_off(); return -1; }

    uint16_t c = (uint16_t)(ent[26] | (ent[27] << 8));
    if (ent[11] & ATTR_DIR)
        rmrf_cluster(c, 0);
    free_chain(c);
    fat_flush();

    uint8_t sec[512];
    read_fs(lba, 1, sec);
    sec[off] = 0xE5;
    write_fs(lba, 1, sec);
    floppy_motor_off();
    return 0;
}

/* ---- cd / pwd ------------------------------------------------------------- */
static void path_push(const char *name) {
    uint32_t l = strlen(cwd_path);
    if (l != 1) cwd_path[l++] = '/';   /* not root */
    for (const char *p = name; *p && l < sizeof(cwd_path) - 1; p++)
        cwd_path[l++] = *p;
    cwd_path[l] = '\0';
}
static void path_pop(void) {
    uint32_t l = strlen(cwd_path);
    while (l > 1 && cwd_path[l - 1] != '/') l--;   /* drop last component */
    if (l > 1) l--;                                 /* drop the slash */
    if (l == 0) l = 1;
    cwd_path[l] = '\0';
    cwd_path[0] = '/';
}

int fs_cd(const char *name) {
    if (!mounted) return -1;

    if (strcmp(name, "/") == 0) {
        cwd_cluster = 0;
        cwd_path[0] = '/'; cwd_path[1] = '\0';
        return 0;
    }
    if (name[0] == '\0' || strcmp(name, ".") == 0)
        return 0;
    if (strcmp(name, "..") == 0) {
        if (cwd_cluster == 0) return 0;
        static const uint8_t dd[11] = {'.','.',' ',' ',' ',' ',' ',' ',' ',' ',' '};
        uint8_t ent[32];
        if (!dir_find(cwd_cluster, dd, 0, 0, ent)) { floppy_motor_off(); return -1; }
        cwd_cluster = (uint16_t)(ent[26] | (ent[27] << 8));
        path_pop();
        floppy_motor_off();
        return 0;
    }

    uint8_t name83[11];
    to_83(name, name83);
    uint8_t ent[32];
    if (!dir_find(cwd_cluster, name83, 0, 0, ent)) {
        console_write("cd: no such directory\n");
        floppy_motor_off();
        return -1;
    }
    if (!(ent[11] & ATTR_DIR)) {
        console_write("cd: not a directory\n");
        floppy_motor_off();
        return -1;
    }
    cwd_cluster = (uint16_t)(ent[26] | (ent[27] << 8));
    path_push(name);
    floppy_motor_off();
    return 0;
}

void fs_pwd(void) {
    console_write(cwd_path);
    console_putc('\n');
}

/* ---- tab completion ------------------------------------------------------- */
static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

int fs_complete(const char *prefix, char *out, uint32_t outsize) {
    char up[13];
    uint32_t pl = 0;
    for (const char *q = prefix; *q && pl < sizeof(up) - 1; q++)
        up[pl++] = upper(*q);
    up[pl] = '\0';

    int matches = 0;
    out[0] = '\0';
    uint8_t sec[512];
    for (uint32_t idx = 0; ; idx++) {
        uint32_t lba = dir_sector_lba(cwd_cluster, idx);
        if (lba == 0 || read_fs(lba, 1, sec) != 0) break;
        for (int e = 0; e < 16; e++) {
            uint8_t *ent = sec + e * 32;
            if (ent[0] == 0x00) goto fin;
            if (ent[0] == 0xE5 || ent[11] == ATTR_LFN || (ent[11] & ATTR_VOLUME))
                continue;
            if (ent[0] == '.') continue;

            char name[13];
            format_name(ent, name);
            int ok = 1;
            for (uint32_t i = 0; i < pl; i++)
                if (name[i] == '\0' || upper(name[i]) != up[i]) { ok = 0; break; }
            if (!ok) continue;

            matches++;
            if (matches == 1) {
                uint32_t i = 0;
                for (; name[i] && i < outsize - 1; i++) out[i] = name[i];
                out[i] = '\0';
            } else {
                uint32_t i = 0;                     /* shrink to common prefix */
                while (out[i] && name[i] && upper(out[i]) == upper(name[i])) i++;
                out[i] = '\0';
            }
        }
    }
fin:
    floppy_motor_off();
    return matches;
}
