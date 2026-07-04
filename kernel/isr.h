/* ===========================================================================
 * PumpkinOS - interrupt service routine frame
 * ---------------------------------------------------------------------------
 * This struct mirrors exactly what isr.asm pushes onto the stack before it
 * calls the C dispatcher. Because the kernel always runs in ring 0, a fault
 * or IRQ does NOT switch stacks, so the CPU pushes only eip/cs/eflags (plus
 * an error code for some exceptions) -- there is no user esp/ss here.
 * ========================================================================= */
#ifndef PUMPKIN_ISR_H
#define PUMPKIN_ISR_H

#include <stdint.h>

struct registers {
    uint32_t ds;                                     /* saved data segment    */
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /* pushed by 'pusha'     */
    uint32_t int_no, err_code;                       /* pushed by our stubs   */
    uint32_t eip, cs, eflags;                        /* pushed by the CPU     */
};

/* Called from isr.asm's common stub. */
void isr_handler(struct registers *r);

#endif /* PUMPKIN_ISR_H */
