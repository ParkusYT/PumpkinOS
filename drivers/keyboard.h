/* ===========================================================================
 * PumpkinOS - PS/2 keyboard driver
 * ========================================================================= */
#ifndef PUMPKIN_KEYBOARD_H
#define PUMPKIN_KEYBOARD_H

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
