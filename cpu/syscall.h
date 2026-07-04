/* ===========================================================================
 * PumpkinOS - system calls (int 0x80)
 * ---------------------------------------------------------------------------
 * Ring-3 code cannot touch hardware or kernel memory, so it asks the kernel
 * for services through the int 0x80 gate. Convention: syscall number in EAX,
 * arguments in EBX/ECX/EDX, return value back in EAX.
 * ========================================================================= */
#ifndef PUMPKIN_SYSCALL_H
#define PUMPKIN_SYSCALL_H

#include "isr.h"

#define SYS_EXIT   0    /* end the calling task                         */
#define SYS_PUTC   1    /* EBX = character to print                     */
#define SYS_WRITE  2    /* EBX = pointer, ECX = length                  */
#define SYS_GETCPL 3    /* -> current privilege level of the caller     */

/* Dispatch an int 0x80, called from the interrupt handler. */
void syscall_dispatch(struct registers *r);

#endif /* PUMPKIN_SYSCALL_H */
