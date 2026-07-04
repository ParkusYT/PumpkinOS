/* ===========================================================================
 * PumpkinOS - low-level x86 port I/O helpers
 * ========================================================================= */
#ifndef PUMPKIN_IO_H
#define PUMPKIN_IO_H

#include <stdint.h>

/* Write a byte to an I/O port. */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Read a byte from an I/O port. */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Tiny delay by doing a throwaway write to an unused port. Some old hardware
 * needs a moment between successive commands to the PIC / PS/2 controller. */
static inline void io_wait(void) {
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

#endif /* PUMPKIN_IO_H */
