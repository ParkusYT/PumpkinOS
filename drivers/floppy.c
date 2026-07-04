/* ===========================================================================
 * PumpkinOS - floppy disk controller driver (1.44 MB, drive 0)
 * ---------------------------------------------------------------------------
 * Classic 82077 programming: reset -> specify timings -> recalibrate, then for
 * each read: seek the cylinder, arm ISA DMA channel 2, issue Read Data, wait
 * for IRQ6, and copy the sector out of the DMA bounce buffer. Geometry is the
 * standard 1.44 MB floppy: 80 cylinders, 2 heads, 18 sectors/track.
 * ========================================================================= */
#include "floppy.h"
#include "io.h"
#include "pic.h"
#include "timer.h"
#include "string.h"
#include <stdint.h>

/* FDC registers (primary controller). */
#define FDC_DOR   0x3F2   /* Digital Output Register  */
#define FDC_MSR   0x3F4   /* Main Status Register     */
#define FDC_FIFO  0x3F5   /* Data FIFO                */
#define FDC_CCR   0x3F7   /* Configuration Control    */

/* FDC commands. */
#define CMD_SPECIFY     0x03
#define CMD_RECALIBRATE 0x07
#define CMD_SENSE_INT   0x08
#define CMD_SEEK        0x0F
#define CMD_READ        0xE6   /* read data + MT + MFM + SK */

/* 1.44 MB geometry. */
#define SPT   18
#define HEADS 2

/* ISA DMA bounce buffer: a fixed low physical address (< 16 MB, 64 KB-aligned
 * so a 512-byte transfer never crosses a 64 KB page). It sits above the kernel
 * (which loads at 0x10000 and grows up) and below the stack at 0x90000, inside
 * the first MiB that the PMM reserves and paging identity-maps. */
#define DMA_BUFFER 0x00080000u

static volatile int irq_fired = 0;
static int          motor_on  = 0;

void floppy_irq(void) {
    irq_fired = 1;
}

/* Wait for the controller's IRQ6. Times out after a few seconds rather than
 * hanging if the hardware never responds. */
static int wait_irq(void) {
    for (int i = 0; i < 400; i++) {
        if (irq_fired) {
            irq_fired = 0;
            return 0;
        }
        __asm__ volatile("sti; hlt");   /* sleep until the next interrupt */
    }
    return -1;
}

/* Push a byte into the FDC once it is ready to accept one (RQM=1, DIO=0). */
static void fdc_write(uint8_t val) {
    for (int i = 0; i < 10000; i++) {
        if ((inb(FDC_MSR) & 0xC0) == 0x80) {
            outb(FDC_FIFO, val);
            return;
        }
        io_wait();
    }
}

/* Read a byte from the FDC once one is available (RQM=1, DIO=1). */
static uint8_t fdc_read(void) {
    for (int i = 0; i < 10000; i++) {
        if ((inb(FDC_MSR) & 0xC0) == 0xC0)
            return inb(FDC_FIFO);
        io_wait();
    }
    return 0;
}

static void ensure_motor(void) {
    if (!motor_on) {
        outb(FDC_DOR, 0x1C);           /* motor0 on, DMA/IRQ, normal, drive 0 */
        timer_sleep(300);              /* spin-up */
        motor_on = 1;
    }
}

void floppy_motor_off(void) {
    outb(FDC_DOR, 0x0C);               /* motors off, DMA/IRQ, drive 0 */
    motor_on = 0;
}

/* Ask the controller for the result of a seek/recalibrate/reset. */
static void sense_interrupt(uint8_t *st0, uint8_t *cyl) {
    fdc_write(CMD_SENSE_INT);
    *st0 = fdc_read();
    *cyl = fdc_read();
}

static int recalibrate(void) {
    ensure_motor();
    fdc_write(CMD_RECALIBRATE);
    fdc_write(0x00);                   /* drive 0 */
    if (wait_irq() != 0)
        return -1;
    uint8_t st0, cyl;
    sense_interrupt(&st0, &cyl);
    return 0;
}

static int seek(uint8_t cyl, uint8_t head) {
    fdc_write(CMD_SEEK);
    fdc_write((uint8_t)(head << 2));   /* head << 2 | drive 0 */
    fdc_write(cyl);
    if (wait_irq() != 0)
        return -1;
    uint8_t st0, rc;
    sense_interrupt(&st0, &rc);
    return 0;
}

void floppy_init(void) {
    pic_unmask(6);                     /* let IRQ6 through */

    /* Reset the controller (this raises an IRQ6). */
    irq_fired = 0;
    outb(FDC_DOR, 0x00);               /* enter reset */
    outb(FDC_DOR, 0x0C);               /* leave reset, DMA/IRQ enabled, drive 0 */
    wait_irq();
    for (int i = 0; i < 4; i++) {      /* clear the 4 drives' interrupt state */
        uint8_t st0, cyl;
        sense_interrupt(&st0, &cyl);
    }

    outb(FDC_CCR, 0x00);               /* 500 kbps data rate (1.44 MB) */

    fdc_write(CMD_SPECIFY);            /* step/head timings */
    fdc_write(0xDF);
    fdc_write(0x02);                   /* HLT, DMA mode (ND=0) */

    recalibrate();
    floppy_motor_off();
}

/* Program ISA DMA channel 2 to receive one sector into DMA_BUFFER. */
static void dma_prepare(void) {
    uint32_t addr  = DMA_BUFFER;
    uint16_t count = 512 - 1;

    outb(0x0A, 0x06);                  /* mask channel 2               */
    outb(0x0C, 0xFF);                  /* clear byte-pointer flip-flop */
    outb(0x04, (uint8_t)(addr & 0xFF));
    outb(0x04, (uint8_t)((addr >> 8) & 0xFF));
    outb(0x81, (uint8_t)((addr >> 16) & 0xFF));   /* page register     */
    outb(0x0C, 0xFF);
    outb(0x05, (uint8_t)(count & 0xFF));
    outb(0x05, (uint8_t)((count >> 8) & 0xFF));
    outb(0x0B, 0x46);                  /* single, read (dev->mem), channel 2 */
    outb(0x0A, 0x02);                  /* unmask channel 2             */
}

int floppy_read_sector(uint32_t lba, uint8_t *buf) {
    uint8_t sector = (uint8_t)(lba % SPT) + 1;
    uint8_t head   = (uint8_t)((lba / SPT) % HEADS);
    uint8_t cyl    = (uint8_t)((lba / SPT) / HEADS);

    ensure_motor();

    for (int attempt = 0; attempt < 4; attempt++) {
        if (seek(cyl, head) != 0)
            continue;

        dma_prepare();
        irq_fired = 0;

        fdc_write(CMD_READ);
        fdc_write((uint8_t)((head << 2)));   /* head << 2 | drive 0 */
        fdc_write(cyl);
        fdc_write(head);
        fdc_write(sector);
        fdc_write(0x02);                     /* sector size code: 512 bytes */
        fdc_write(SPT);                      /* last sector in track        */
        fdc_write(0x1B);                     /* gap length                  */
        fdc_write(0xFF);                     /* DTL (unused for 512)        */

        if (wait_irq() != 0)
            continue;

        uint8_t st0 = fdc_read();
        (void)fdc_read();                    /* st1  */
        (void)fdc_read();                    /* st2  */
        (void)fdc_read();                    /* cyl  */
        (void)fdc_read();                    /* head */
        (void)fdc_read();                    /* sect */
        (void)fdc_read();                    /* size */

        if ((st0 & 0xC0) == 0) {             /* IC = normal termination */
            memcpy(buf, (const void *)DMA_BUFFER, 512);
            return 0;
        }
        recalibrate();                       /* error: recalibrate and retry */
    }
    return -1;
}
