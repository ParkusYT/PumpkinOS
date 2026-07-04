/* ===========================================================================
 * PumpkinOS - 8259A Programmable Interrupt Controller
 * ========================================================================= */
#ifndef PUMPKIN_PIC_H
#define PUMPKIN_PIC_H

/* Remap the master/slave PICs so hardware IRQs 0..15 arrive as CPU vectors
 * 32..47 (the first 32 vectors are reserved for CPU exceptions). */
void pic_remap(void);

/* Signal End-Of-Interrupt for the given IRQ line. */
void pic_send_eoi(int irq);

/* Enable (unmask) / disable (mask) a single IRQ line. */
void pic_unmask(int irq);
void pic_mask(int irq);

#endif /* PUMPKIN_PIC_H */
