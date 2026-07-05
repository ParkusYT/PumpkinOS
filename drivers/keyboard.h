/* ===========================================================================
 * PumpkinOS - PS/2 keyboard driver
 * ========================================================================= */
#ifndef PUMPKIN_KEYBOARD_H
#define PUMPKIN_KEYBOARD_H

/* Special key codes returned by keyboard_getchar() (control-range values a
 * normal shell line never contains). */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_HOME  0x15
#define KEY_END   0x16
#define KEY_PGUP  0x17
#define KEY_PGDN  0x18
#define KEY_DEL   0x19

/* Drain the controller and unmask the keyboard IRQ. Call after idt_init()
 * and pic_remap(), before enabling interrupts. */
void keyboard_init(void);

/* IRQ1 handler: reads a scancode, translates it, and enqueues the character.
 * Invoked from the interrupt dispatcher in idt.c. */
void keyboard_irq(void);

/* Non-zero if a translated character is waiting. */
int keyboard_haschar(void);

/* Block (sleeping via hlt) until a character is available, then return it. */
char keyboard_getchar(void);

#endif /* PUMPKIN_KEYBOARD_H */
