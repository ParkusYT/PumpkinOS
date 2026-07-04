/* ===========================================================================
 * PumpkinOS - real-mode BIOS call thunk
 * ---------------------------------------------------------------------------
 * bios_int() drops from 32-bit paged protected mode down to real mode, runs a
 * single BIOS interrupt, and returns to protected mode with the register
 * results. This is what lets the kernel reach the VESA BIOS (int 0x10) to set
 * a high-resolution linear-framebuffer mode on real hardware.
 *
 * The register block layout matches the order the thunk's assembly expects
 * (a pusha-style frame followed by the segment registers and flags).
 * ========================================================================= */
#ifndef PUMPKIN_BIOS_H
#define PUMPKIN_BIOS_H

#include <stdint.h>

typedef struct {
    uint16_t di, si, bp, sp, bx, dx, cx, ax;
    uint16_t gs, fs, es, ds, flags;
} __attribute__((packed)) regs16_t;

/* Invoke real-mode interrupt 'intno' with the given register state; results
 * (including AX and the carry flag) are written back into *regs. Interrupts
 * are disabled for the duration; the caller should mask the PIC first if the
 * BIOS might enable interrupts (the PIC is remapped away from real-mode
 * vectors). */
void bios_int(uint8_t intno, regs16_t *regs);

#endif /* PUMPKIN_BIOS_H */
