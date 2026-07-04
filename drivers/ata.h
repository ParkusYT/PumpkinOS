/* ===========================================================================
 * PumpkinOS - ATA/IDE hard disk driver (28-bit LBA, PIO mode)
 * ---------------------------------------------------------------------------
 * Talks to hard disks on the two legacy IDE channels (primary 0x1F0,
 * secondary 0x170). At boot it IDENTIFYs all four possible drives (each
 * channel's master and slave) and remembers the ones that answer, along with
 * their model string and capacity. Reads and writes go through the data
 * register a word at a time (polled PIO) - no DMA, no interrupts, which keeps
 * it simple and dependable on the old hardware PumpkinOS targets.
 * ========================================================================= */
#ifndef PUMPKIN_ATA_H
#define PUMPKIN_ATA_H

#include <stdint.h>

#define ATA_MAX_DRIVES 4          /* primary + secondary, master + slave */
#define ATA_SECTOR_SIZE 512

/* One detected disk. 'model' is the trimmed IDENTIFY model string; 'sectors'
 * is the 28-bit LBA capacity in 512-byte sectors. */
struct ata_drive {
    int      present;
    int      channel;             /* 0 = primary, 1 = secondary */
    int      slave;               /* 0 = master, 1 = slave      */
    uint32_t sectors;             /* total addressable sectors (LBA28) */
    char     model[41];           /* 40 chars + NUL */
};

/* Probe both IDE channels and record every drive that responds. Pure polled
 * PIO, so it is safe to call before interrupts are enabled. */
void ata_init(void);

/* Number of hard disks found, and read-only access to a drive's info by index
 * (0 .. ata_drive_count()-1). Returns NULL for an out-of-range index. */
int ata_drive_count(void);
const struct ata_drive *ata_get_drive(int index);

/* Read / write 'count' consecutive 512-byte sectors starting at LBA 'lba' on
 * disk 'index'. Return 0 on success, -1 on error (bad drive, bad range, or a
 * controller error). 'count' of 0 is treated as 256 by the hardware; pass 1..255. */
int ata_read_sectors(int index, uint32_t lba, uint8_t count, void *buf);
int ata_write_sectors(int index, uint32_t lba, uint8_t count, const void *buf);

#endif /* PUMPKIN_ATA_H */
