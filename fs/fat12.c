/* ===========================================================================
 * PumpkinOS - FAT12 filesystem (read-only)
 * ========================================================================= */
#include "fat12.h"
#include "console.h"
#include "string.h"
#include <stdint.h>

/* Must match RAMDISK_SEG in boot/boot.asm (0x3000:0 = physical 0x30000). */
#define RAMDISK_BASE 0x30000u

#define ATTR_LFN    0x0F
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10

static uint8_t *disk;                /* base of the in-memory FAT12 image */
static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint16_t reserved_sectors;
static uint8_t  num_fats;
static uint16_t root_entries;
static uint16_t sectors_per_fat;
static uint32_t fat_start;           /* sector of the first FAT           */
static uint32_t root_start;          /* sector of the root directory      */
static uint32_t data_start;          /* sector of cluster 2               */
static int      mounted;

static uint16_t rd16(uint32_t off) {
    return (uint16_t)(disk[off] | (disk[off + 1] << 8));
}

static uint8_t *sector_ptr(uint32_t lba) {
    return disk + lba * bytes_per_sector;
}

/* The 12-bit FAT entry for a cluster (clusters are packed 1.5 bytes each). */
static uint16_t fat_entry(uint16_t cluster) {
    uint32_t off = fat_start * bytes_per_sector + cluster + (cluster / 2);
    uint16_t v = (uint16_t)(disk[off] | (disk[off + 1] << 8));
    return (cluster & 1) ? (v >> 4) : (v & 0x0FFF);
}

int fs_mounted(void) { return mounted; }

int fs_init(void) {
    disk    = (uint8_t *)RAMDISK_BASE;
    mounted = 0;

    bytes_per_sector    = rd16(11);
    sectors_per_cluster = disk[13];
    reserved_sectors    = rd16(14);
    num_fats            = disk[16];
    root_entries        = rd16(17);
    sectors_per_fat     = rd16(22);

    /* Basic sanity: a real FAT12 boot sector for us has 512-byte sectors. */
    if (bytes_per_sector != 512 || sectors_per_cluster == 0 ||
        num_fats == 0 || num_fats > 2)
        return -1;

    fat_start  = reserved_sectors;
    root_start = reserved_sectors + (uint32_t)num_fats * sectors_per_fat;
    uint32_t root_sectors =
        ((uint32_t)root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;
    data_start = root_start + root_sectors;
    mounted    = 1;

    /* Count the files so the caller can report it. */
    int files = 0;
    uint8_t *dir = sector_ptr(root_start);
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t *e = dir + i * 32;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == ATTR_LFN || (e[11] & ATTR_VOLUME))
            continue;
        files++;
    }
    return files;
}

/* Format an 8.3 directory entry name into "NAME.EXT". */
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

    uint8_t *dir = sector_ptr(root_start);
    int files = 0;

    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t *e = dir + i * 32;
        if (e[0] == 0x00)
            break;                        /* end of directory */
        if (e[0] == 0xE5 || e[11] == ATTR_LFN || (e[11] & ATTR_VOLUME))
            continue;                     /* deleted / long name / volume label */

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

    uint8_t *dir = sector_ptr(root_start);
    for (uint32_t i = 0; i < root_entries; i++) {
        uint8_t *e = dir + i * 32;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5 || e[11] == ATTR_LFN || (e[11] & (ATTR_VOLUME | ATTR_DIR)))
            continue;
        if (memcmp(e, want, 11) != 0)
            continue;

        /* Found it: follow the cluster chain, streaming its bytes. */
        uint16_t cluster = (uint16_t)(e[26] | (e[27] << 8));
        uint32_t remaining = e[28] | (e[29] << 8) | (e[30] << 16) |
                             ((uint32_t)e[31] << 24);
        uint32_t cluster_bytes = (uint32_t)sectors_per_cluster * bytes_per_sector;
        int guard = 0;

        while (cluster >= 2 && cluster < 0xFF8 && remaining > 0 && guard++ < 4096) {
            uint32_t lba = data_start + (uint32_t)(cluster - 2) * sectors_per_cluster;
            uint8_t *cp = sector_ptr(lba);
            uint32_t n = remaining < cluster_bytes ? remaining : cluster_bytes;
            for (uint32_t k = 0; k < n; k++)
                console_putc((char)cp[k]);
            remaining -= n;
            cluster = fat_entry(cluster);
        }
        return 0;
    }
    return -1;                            /* not found */
}
