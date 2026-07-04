/* ===========================================================================
 * PumpkinOS - 8259A Programmable Interrupt Controller
 * ========================================================================= */
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI   0x20   /* End-Of-Interrupt command */

#define ICW1_INIT 0x11   /* start init, expect ICW4 */
#define ICW4_8086 0x01   /* 8086/88 mode            */

#define PIC1_VECTOR 0x20 /* master IRQs -> vectors 32..39 */
#define PIC2_VECTOR 0x28 /* slave  IRQs -> vectors 40..47 */

void pic_remap(void) {
    /* Save the current masks so we can restore them afterwards. */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* Start the initialisation sequence (ICW1). */
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();

    /* ICW2: vector offsets. */
    outb(PIC1_DATA, PIC1_VECTOR); io_wait();
    outb(PIC2_DATA, PIC2_VECTOR); io_wait();

    /* ICW3: tell master there is a slave on IRQ2; tell slave its cascade id. */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Restore the saved masks. */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(int irq) {
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);   /* slave first, then always the master */
    outb(PIC1_CMD, PIC_EOI);
}

void pic_unmask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : irq - 8;
    outb(port, inb(port) & ~(1 << bit));
}

void pic_mask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : irq - 8;
    outb(port, inb(port) | (1 << bit));
}
