/* ===========================================================================
 * PumpkinOS - FAT12 filesystem (read-only, over the real floppy)
 * ---------------------------------------------------------------------------
 * The FAT12 volume lives on the floppy starting at a fixed sector (after the
 * boot sector and the kernel). This driver reads it through the floppy
 * controller: it caches the BPB, the FAT, and the root directory at mount
 * time, and streams file data straight off the disk on demand.
 * ========================================================================= */
#include "fat12.h"
#include "floppy.h"
#include "console.h"
#include "kheap.h"
#include "string.h"
#include <stdint.h>

/* The whole floppy is one FAT12 volume now (boot sector + kernel live inside
 * it as sector 0 and the KERNEL.BIN file), so the volume starts at LBA 0. */
#define FS_BASE_LBA 0u

#define ATTR_LFN    0x0F
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10

static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint16_t reserved_sectors;
static uint8_t  num_fats;
static uint16_t root_entries;
static uint16_t sectors_per_fat;
static uint32_t fat_start;           /* volume-relative sector of FAT 1     */
static uint32_t root_start;          /* volume-relative sector of root dir  */
static uint32_t root_sectors;
static uint32_t data_start;          /* volume-relative sector of cluster 2 */

static uint8_t *fat_cache;           /* whole FAT, cached at mount time     */
static uint8_t *root_cache;          /* whole root directory                */
static uint8_t *cluster_buf;         /* one-cluster scratch for reads       */
static int      mounted;

static uint16_t rd16(const uint8_t *p, uint32_t off) {
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}

/* Read 'count' volume-relative sectors into buf via the floppy driver. */
static int read_fs(uint32_t fs_lba, uint32_t count, uint8_t *buf) {
    for (uint32_t i = 0; i < count; i++)
        if (floppy_read_sector(FS_BASE_LBA + fs_lba + i, buf + i * 512) != 0)
            return -1;
    return 0;
}

/* The 12-bit FAT entry for a cluster (entries are packed 1.5 bytes each). */
static uint16_t fat_entry(uint16_t cluster) {
    uint32_t off = cluster + (cluster / 2);
    uint16_t v = (uint16_t)(fat_cache[off] | (fat_cache[off + 1] << 8));
    return (cluster & 1) ? (v >> 4) : (v & 0x0FFF);
}

int fs_mounted(void) { return mounted; }

int fs_init(void) {
    mounted = 0;

    uint8_t bpb[512];
    if (floppy_read_sector(FS_BASE_LBA, bpb) != 0) {
        floppy_motor_off();
        return -1;
    }

    bytes_per_sector    = rd16(bpb, 11);
    sectors_per_cluster = bpb[13];
    reserved_sectors    = rd16(bpb, 14);
    num_fats            = bpb[16];
    root_entries        = rd16(bpb, 17);
    sectors_per_fat     = rd16(bpb, 22);

    if (bytes_per_sector != 512 || sectors_per_cluster == 0 ||
        num_fats == 0 || num_fats > 2) {
        floppy_motor_off();
        return -1;
    }

    fat_start    = reserved_sectors;
    root_start   = reserved_sectors + (uint32_t)num_fats * sectors_per_fat;
    root_sectors = ((uint32_t)root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;
    data_start   = root_start + root_sectors;

    /* Cache the FAT and the root directory (small; heap-allocated). */
    fat_cache   = (uint8_t *)kmalloc((uint32_t)sectors_per_fat * 512);
    root_cache  = (uint8_t *)kmalloc(root_sectors * 512);
    cluster_buf = (uint8_t *)kmalloc((uint32_t)sectors_per_cluster * 512);
    if (!fat_cache || !root_cache || !cluster_buf) {
        floppy_motor_off();
        return -1;
    }
    if (read_fs(fat_start, sectors_per_fat, fat_cache) != 0 ||
        read_fs(root_start, root_sectors, root_cache) != 0) {
        floppy_motor_off();
        return -1;
    }
    floppy_motor_off();
    mounted = 1;

    int files = 0;
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t *e = root_cache + i * 32;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == ATTR_LFN || (e[11] & ATTR_VOLUME)) continue;
        files++;
    }
    return files;
}

static void format_name(const uint8_t *e, char *out) {
    int n = 0;
    for (int j = 0; j < 8 && e[j] != ' '; j++)
        out[n++] = (char)e[j];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int j = 8; j < 11 && e[j] != ' '; j++)
            out[n++] = (char)e[j];
    }
    out[n] = '\0';
}

void fs_list(void) {
    if (!mounted) {
        console_write("ls: no filesystem mounted\n");
        return;
    }

    int files = 0;
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t *e = root_cache + i * 32;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == ATTR_LFN || (e[11] & ATTR_VOLUME)) continue;

        char name[13];
        format_name(e, name);
        uint32_t size = e[28] | (e[29] << 8) | (e[30] << 16) |
                        ((uint32_t)e[31] << 24);

        console_write("  ");
        console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        int w = 0;
        for (const char *p = name; *p; p++, w++)
            console_putc(*p);
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        while (w++ < 14)
            console_putc(' ');
        console_write_dec(size);
        console_write(" bytes\n");
        files++;
    }

    console_write("  ");
    console_write_dec((uint32_t)files);
    console_write(files == 1 ? " file\n" : " files\n");
}

/* Turn "readme.txt" into the padded uppercase 8.3 form "README  TXT". */
static void to_83(const char *name, uint8_t *out) {
    for (int i = 0; i < 11; i++)
        out[i] = ' ';
    int i = 0;
    while (*name && *name != '.' && i < 8) {
        char c = *name++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i++] = (uint8_t)c;
    }
    while (*name && *name != '.')
        name++;
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

int fs_cat(const char *name) {
    if (!mounted) {
        console_write("cat: no filesystem mounted\n");
        return 0;
    }

    uint8_t want[11];
    to_83(name, want);

    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t *e = root_cache + i * 32;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == ATTR_LFN || (e[11] & (ATTR_VOLUME | ATTR_DIR)))
            continue;
        if (memcmp(e, want, 11) != 0)
            continue;

        /* Found it: walk the cluster chain, reading each cluster off the disk. */
        uint16_t cluster = (uint16_t)(e[26] | (e[27] << 8));
        uint32_t remaining = e[28] | (e[29] << 8) | (e[30] << 16) |
                             ((uint32_t)e[31] << 24);
        uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * bytes_per_sector;
        int guard = 0;

        while (cluster >= 2 && cluster < 0xFF8 && remaining > 0 && guard++ < 4096) {
            uint32_t lba = data_start + (uint32_t)(cluster - 2) * sectors_per_cluster;
            if (read_fs(lba, sectors_per_cluster, cluster_buf) != 0) {
                console_write("\ncat: read error\n");
                break;
            }
            uint32_t n = remaining < cluster_bytes ? remaining : cluster_bytes;
            for (uint32_t k = 0; k < n; k++)
                console_putc((char)cluster_buf[k]);
            remaining -= n;
            cluster = fat_entry(cluster);
        }
        floppy_motor_off();
        return 0;
    }
    return -1;                            /* not found */
}
