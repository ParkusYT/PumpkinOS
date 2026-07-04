/* ===========================================================================
 * PumpkinOS - PS/2 mouse driver
 * ---------------------------------------------------------------------------
 * The 8042 controller's auxiliary port raises IRQ12 with 3-byte movement
 * packets. We track an absolute cursor position (clamped to the 320x200
 * screen) and the button state, and bump an event counter so the desktop can
 * tell when something changed.
 * ========================================================================= */
#ifndef PUMPKIN_MOUSE_H
#define PUMPKIN_MOUSE_H

#include <stdint.h>

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

/* Enable the aux device and IRQ12. Call after idt_init()/pic_remap(). */
void mouse_init(void);

/* IRQ12 handler: assembles a packet and updates position/buttons. */
void mouse_irq(void);

int mouse_x(void);
int mouse_y(void);
int mouse_buttons(void);        /* bitmask of MOUSE_* */

/* Increments on every processed packet - lets a UI loop detect activity. */
uint32_t mouse_seq(void);

#endif /* PUMPKIN_MOUSE_H */
