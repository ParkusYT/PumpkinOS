/* ===========================================================================
 * PumpkinOS - Global Descriptor Table + Task State Segment
 * ---------------------------------------------------------------------------
 * The bootloader gave us a minimal flat GDT (null / kernel code / kernel data).
 * To run code at ring 3 we need user code/data descriptors and a TSS, which is
 * where the CPU finds the ring-0 stack to switch to when a user task traps.
 * ========================================================================= */
#ifndef PUMPKIN_GDT_H
#define PUMPKIN_GDT_H

#include <stdint.h>

/* Segment selectors (OR user ones with 3 for the ring-3 RPL). */
#define KERNEL_CODE_SEL 0x08
#define KERNEL_DATA_SEL 0x10
#define USER_CODE_SEL   0x18
#define USER_DATA_SEL   0x20
#define TSS_SEL         0x28

/* Install the kernel GDT and load the TSS. */
void gdt_init(void);

/* Set the ring-0 stack the CPU will switch to on the next trap from ring 3. */
void tss_set_esp0(uint32_t esp0);

#endif /* PUMPKIN_GDT_H */
