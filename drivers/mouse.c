/* ===========================================================================
 * PumpkinOS - PS/2 mouse driver (IRQ12)
 * ========================================================================= */
#include "mouse.h"
#include "pic.h"
#include "io.h"
#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

/* Screen bounds the cursor is clamped to (set by the desktop for its mode). */
static volatile int      bound_w = 320, bound_h = 200;
static volatile int      mx = 160, my = 100;
static volatile int      buttons;
static volatile uint32_t seq;

void mouse_set_bounds(int w, int h) {
    bound_w = w;
    bound_h = h;
    mx = w / 2;
    my = h / 2;
}

static uint8_t  packet[3];
static int      phase;

/* Wait until the controller can accept a write (input buffer empty). */
static void wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if ((inb(PS2_STATUS) & 0x02) == 0)
            return;
}
/* Wait until the controller has a byte for us (output buffer full). */
static void wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & 0x01)
            return;
}

/* Send a command byte to the mouse (prefixed with 0xD4) and read its ACK. */
static void mouse_command(uint8_t cmd) {
    wait_write(); outb(PS2_CMD, 0xD4);
    wait_write(); outb(PS2_DATA, cmd);
    wait_read();  (void)inb(PS2_DATA);      /* ACK (0xFA) */
}

void mouse_init(void) {
    wait_write(); outb(PS2_CMD, 0xA8);      /* enable the auxiliary device */

    /* Turn on IRQ12 in the controller command byte (bit 1), and make sure the
     * mouse clock is enabled (clear bit 5). */
    wait_write(); outb(PS2_CMD, 0x20);      /* request command byte */
    wait_read();
    uint8_t status = inb(PS2_DATA);
    status |= 0x02;
    status &= ~0x20;
    wait_write(); outb(PS2_CMD, 0x60);      /* write command byte */
    wait_write(); outb(PS2_DATA, status);

    mouse_command(0xF6);                     /* set defaults              */
    mouse_command(0xF4);                     /* enable data reporting     */

    pic_unmask(2);                           /* cascade line to the slave */
    pic_unmask(12);                          /* the mouse IRQ             */
}

/* Feed one byte into the 3-byte packet state machine. */
static void feed(uint8_t data) {
    switch (phase) {
    case 0:
        /* First byte always has bit 3 set; if not, we're out of sync. */
        if (!(data & 0x08))
            return;
        packet[0] = data;
        phase = 1;
        break;
    case 1:
        packet[1] = data;
        phase = 2;
        break;
    case 2:
        packet[2] = data;
        phase = 0;

        if (packet[0] & 0xC0)                /* X/Y overflow: drop packet */
            break;

        int dx = packet[1];
        int dy = packet[2];
        if (packet[0] & 0x10) dx -= 256;     /* sign-extend */
        if (packet[0] & 0x20) dy -= 256;

        int nx = mx + dx;
        int ny = my - dy;                    /* screen Y grows downward */
        if (nx < 0) nx = 0; else if (nx >= bound_w) nx = bound_w - 1;
        if (ny < 0) ny = 0; else if (ny >= bound_h) ny = bound_h - 1;
        mx = nx;
        my = ny;
        buttons = packet[0] & 0x07;
        seq++;
        break;
    }
}

void mouse_irq(void) {
    /* One IRQ may cover several queued bytes, so drain the whole packet. Stop
     * at a byte that isn't from the aux device (status bit 5) so we never steal
     * the keyboard's data. */
    while (inb(PS2_STATUS) & 0x01) {
        if (!(inb(PS2_STATUS) & 0x20))
            break;
        feed(inb(PS2_DATA));
    }
}

int mouse_x(void)       { return mx; }
int mouse_y(void)       { return my; }
int mouse_buttons(void) { return buttons; }
uint32_t mouse_seq(void){ return seq; }
