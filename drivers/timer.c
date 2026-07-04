/* ===========================================================================
 * PumpkinOS - Programmable Interval Timer (8253/8254 PIT)
 * ---------------------------------------------------------------------------
 * The PIT drives a fixed clock (1193182 Hz) through a 16-bit divisor to
 * produce a periodic signal on IRQ0. We divide it down to a chosen tick
 * frequency (100 Hz => one tick every 10 ms) and count the ticks, giving the
 * kernel a sense of elapsed time for uptime and sleeping.
 * ========================================================================= */
#include "timer.h"
#include "pic.h"
#include "io.h"

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43
#define PIT_BASE_FREQ 1193182u   /* the PIT's fixed input frequency, in Hz */

static volatile uint32_t ticks = 0;
static uint32_t          frequency = 0;

void timer_init(uint32_t frequency_hz) {
    if (frequency_hz == 0)
        frequency_hz = 100;
    frequency = frequency_hz;

    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;          /* clamp to the 16-bit counter */

    /* 0x36 = channel 0, access lo-byte then hi-byte, mode 3 (square wave),
     * binary counting. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    pic_unmask(0);                 /* let IRQ0 through */
}

void timer_irq(void) {
    ticks++;
}

uint32_t timer_ticks(void) {
    return ticks;
}

uint32_t timer_hz(void) {
    return frequency;
}

void timer_sleep(uint32_t ms) {
    if (frequency == 0)
        return;

    uint32_t needed = (ms * frequency) / 1000u;
    if (needed == 0)
        needed = 1;                /* always wait at least one tick */

    uint32_t target = ticks + needed;
    while (ticks < target)
        __asm__ volatile("sti; hlt");   /* idle until the next interrupt */
}
