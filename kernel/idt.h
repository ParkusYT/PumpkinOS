/* ===========================================================================
 * PumpkinOS - Interrupt Descriptor Table
 * ========================================================================= */
#ifndef PUMPKIN_IDT_H
#define PUMPKIN_IDT_H

/* Build the IDT, point every vector at the right stub, and load it. */
void idt_init(void);

#endif /* PUMPKIN_IDT_H */
