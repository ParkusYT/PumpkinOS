/* ===========================================================================
 * PumpkinOS - floppy disk controller (8272A/82077) driver
 * ---------------------------------------------------------------------------
 * Reads 512-byte sectors off the real floppy drive using the FDC plus ISA DMA
 * channel 2. This is what lets the kernel read the boot disk directly, rather
 * than a RAM copy pre-loaded by the bootloader.
 * ========================================================================= */
#ifndef PUMPKIN_FLOPPY_H
#define PUMPKIN_FLOPPY_H

#include <stdint.h>

/* Reset + recalibrate the controller. Must be called AFTER interrupts are
 * enabled (it waits on IRQ6 and uses the timer for motor spin-up). */
void floppy_init(void);

/* IRQ6 handler: records that the controller finished a command. */
void floppy_irq(void);

/* Read/write one LBA sector (512 bytes). Return 0 on success, -1 on error.
 * Both leave the motor spinning; call floppy_motor_off() when done. */
int floppy_read_sector(uint32_t lba, uint8_t *buf);
int floppy_write_sector(uint32_t lba, const uint8_t *buf);

/* Stop the drive motor. */
void floppy_motor_off(void);

#endif /* PUMPKIN_FLOPPY_H */
