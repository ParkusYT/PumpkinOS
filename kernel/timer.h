/* ===========================================================================
 * PumpkinOS - Programmable Interval Timer (8253/8254 PIT)
 * ========================================================================= */
#ifndef PUMPKIN_TIMER_H
#define PUMPKIN_TIMER_H

#include <stdint.h>

/* Program PIT channel 0 to fire IRQ0 at the given frequency and unmask it. */
void timer_init(uint32_t frequency_hz);

/* IRQ0 handler: bumps the tick counter. Invoked from the IRQ dispatcher. */
void timer_irq(void);

/* Ticks elapsed since timer_init(). */
uint32_t timer_ticks(void);

/* The configured tick frequency (ticks per second). */
uint32_t timer_hz(void);

/* Block for roughly the given number of milliseconds, sleeping via hlt. */
void timer_sleep(uint32_t ms);

#endif /* PUMPKIN_TIMER_H */
