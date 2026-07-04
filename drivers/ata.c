/* ===========================================================================
 * PumpkinOS - ATA/IDE hard disk driver (28-bit LBA, PIO mode)
 * ---------------------------------------------------------------------------
 * Classic "ATA task file" programming. Each channel exposes eight command
 * registers at its base port plus a control/alt-status register a bit higher:
 *
 *   base+0  data (16-bit)     base+4  LBA mid / cyl low
 *   base+1  error/features    base+5  LBA high / cyl high
 *   base+2  sector count      base+6  drive/head select
 *   base+3  LBA low           base+7  status (read) / command (write)
 *   ctrl+0  alt status (read) / device control (write)
 *
 * Detection issues IDENTIFY DEVICE (0xEC) to each of the four drives and, for
 * the ones that answer as ATA disks, pulls the model string and LBA28 sector
 * count out of the 256-word identification block. Transfers poll BSY/DRQ in
 * the status register and move data a word at a time.
 * ========================================================================= */
#include "ata.h"
#include "io.h"
#include "console.h"
#include "string.h"
#include <stdint.h>

/* Channel I/O bases and their control ports. */
#define PRI_BASE  0x1F0
#define PRI_CTRL  0x3F6
#define SEC_BASE  0x170
#define SEC_CTRL  0x376

/* Register offsets from a channel base. */
#define REG_DATA      0
#define REG_ERROR     1
#define REG_FEATURES  1
#define REG_SECCOUNT  2
#define REG_LBA0      3
#define REG_LBA1      4
#define REG_LBA2      5
#define REG_HDDEVSEL  6
#define REG_STATUS    7
#define REG_COMMAND   7

/* Status register bits. */
#define ST_ERR  0x01   /* error         */
#define ST_DRQ  0x08   /* data request  */
#define ST_SRV  0x10   /* overlapped     */
#define ST_DF   0x20   /* drive fault   */
#define ST_RDY  0x40   /* ready         */
#define ST_BSY  0x80   /* busy          */

/* Commands. */
#define CMD_READ_PIO   0x20
#define CMD_WRITE_PIO  0x30
#define CMD_CACHE_FLUSH 0xE7
#define CMD_IDENTIFY   0xEC

static struct ata_drive drives[ATA_MAX_DRIVES];
static int              drive_count;

static uint16_t channel_base(int channel) {
    return channel ? SEC_BASE : PRI_BASE;
}
static uint16_t channel_ctrl(int channel) {
    return channel ? SEC_CTRL : PRI_CTRL;
}

/* Reading the alt-status register four times is the standard ~400ns settle
 * delay after selecting a drive or issuing a command. */
static void ata_delay(int channel) {
    uint16_t ctrl = channel_ctrl(channel);
    for (int i = 0; i < 4; i++)
        (void)inb(ctrl);
}

/* Spin until BSY clears, with a generous timeout so a dead/absent controller
 * can't hang the boot. Returns the final status byte (BSY may still be set on
 * timeout - the caller checks). */
static uint8_t wait_not_busy(uint16_t base) {
    uint8_t status = 0;
    for (uint32_t i = 0; i < 4000000u; i++) {
        status = inb(base + REG_STATUS);
        if (!(status & ST_BSY))
            return status;
    }
    return status;
}

/* Wait for DRQ (or an error). Returns 0 when data is ready, -1 on ERR/DF or
 * timeout. */
static int wait_drq(uint16_t base) {
    for (uint32_t i = 0; i < 4000000u; i++) {
        uint8_t status = inb(base + REG_STATUS);
        if (status & (ST_ERR | ST_DF))
            return -1;
        if (!(status & ST_BSY) && (status & ST_DRQ))
            return 0;
    }
    return -1;
}

/* Select master/slave on a channel (0xA0 = master, 0xB0 = slave) and let it
 * settle. The low nibble of 0xE0/0xF0 (LBA mode) is set per-transfer. */
static void select_drive(int channel, int slave) {
    uint16_t base = channel_base(channel);
    outb(base + REG_HDDEVSEL, slave ? 0xB0 : 0xA0);
    ata_delay(channel);
}

/* IDENTIFY one drive. On success fill 'd' and return 1; return 0 if nothing is
 * there or it isn't a plain ATA disk. */
static int identify(int channel, int slave, struct ata_drive *d) {
    uint16_t base = channel_base(channel);

    select_drive(channel, slave);

    /* Zero the address registers, as the IDENTIFY protocol expects. */
    outb(base + REG_SECCOUNT, 0);
    outb(base + REG_LBA0, 0);
    outb(base + REG_LBA1, 0);
    outb(base + REG_LBA2, 0);

    outb(base + REG_COMMAND, CMD_IDENTIFY);
    ata_delay(channel);

    /* Status 0 => no drive present (a floating bus reads 0xFF, also handled
     * because BSY never clears / signatures mismatch below). */
    uint8_t status = inb(base + REG_STATUS);
    if (status == 0 || status == 0xFF)
        return 0;

    status = wait_not_busy(base);
    if (status & ST_BSY)
        return 0;

    /* A non-zero LBA mid/high signature means this is a packet device (ATAPI)
     * or SATA, not a plain ATA disk - skip it. */
    if (inb(base + REG_LBA1) != 0 || inb(base + REG_LBA2) != 0)
        return 0;

    if (wait_drq(base) != 0)
        return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(base + REG_DATA);

    d->present = 1;
    d->channel = channel;
    d->slave   = slave;

    /* LBA28 total sector count lives in words 60-61 (little-endian dword). */
    d->sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);

    /* Model string: words 27-46, each word big-endian (high byte first). */
    for (int i = 0; i < 20; i++) {
        d->model[i * 2]     = (char)(id[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->model[40] = '\0';
    /* Trim trailing spaces the spec pads the field with. */
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = '\0';

    return 1;
}

void ata_init(void) {
    drive_count = 0;
    memset(drives, 0, sizeof(drives));

    for (int channel = 0; channel < 2; channel++) {
        for (int slave = 0; slave < 2; slave++) {
            struct ata_drive d;
            memset(&d, 0, sizeof(d));
            if (identify(channel, slave, &d))
                drives[drive_count++] = d;
        }
    }
}

int ata_drive_count(void) {
    return drive_count;
}

const struct ata_drive *ata_get_drive(int index) {
    if (index < 0 || index >= drive_count)
        return NULL;
    return &drives[index];
}

/* Set up the task file for an LBA28 transfer of 'count' sectors at 'lba' and
 * wait for the controller to become ready. Returns the channel base, or 0 on
 * a bad request. */
static uint16_t setup_transfer(int index, uint32_t lba, uint8_t count) {
    const struct ata_drive *d = ata_get_drive(index);
    if (!d)
        return 0;
    if (lba + count > d->sectors || (lba + count) < lba)
        return 0;                         /* out of range / overflow */

    uint16_t base = channel_base(d->channel);

    if (wait_not_busy(base) & ST_BSY)
        return 0;

    /* Drive select with LBA mode (0xE0 master / 0xF0 slave) and the top 4 LBA
     * bits in the low nibble. */
    outb(base + REG_HDDEVSEL,
         (uint8_t)((d->slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F)));
    ata_delay(d->channel);

    outb(base + REG_FEATURES, 0);
    outb(base + REG_SECCOUNT, count);
    outb(base + REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(base + REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    return base;
}

int ata_read_sectors(int index, uint32_t lba, uint8_t count, void *buf) {
    uint16_t base = setup_transfer(index, lba, count);
    if (!base)
        return -1;

    outb(base + REG_COMMAND, CMD_READ_PIO);

    uint16_t *out = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq(base) != 0)
            return -1;
        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++)
            *out++ = inw(base + REG_DATA);
        ata_delay(index >= 0 ? drives[index].channel : 0);
    }
    return 0;
}

int ata_write_sectors(int index, uint32_t lba, uint8_t count, const void *buf) {
    uint16_t base = setup_transfer(index, lba, count);
    if (!base)
        return -1;

    outb(base + REG_COMMAND, CMD_WRITE_PIO);

    const uint16_t *in = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq(base) != 0)
            return -1;
        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++)
            outw(base + REG_DATA, *in++);
        ata_delay(drives[index].channel);
    }

    /* Flush the drive's write cache so the data is committed. */
    outb(base + REG_COMMAND, CMD_CACHE_FLUSH);
    if (wait_not_busy(base) & ST_BSY)
        return -1;
    return 0;
}
