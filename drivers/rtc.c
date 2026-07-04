/* ===========================================================================
 * PumpkinOS - CMOS real-time clock
 * ---------------------------------------------------------------------------
 * Reads the date/time from the CMOS/RTC chip via ports 0x70 (index) and 0x71
 * (data). Reads twice and compares to avoid catching a value mid-update, then
 * converts from BCD / 12-hour if the status register says so.
 * ========================================================================= */
#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd2bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static void read_raw(uint8_t *s, uint8_t *m, uint8_t *h,
                     uint8_t *d, uint8_t *mo, uint8_t *y) {
    while (update_in_progress())
        ;
    *s  = cmos_read(0x00);
    *m  = cmos_read(0x02);
    *h  = cmos_read(0x04);
    *d  = cmos_read(0x07);
    *mo = cmos_read(0x08);
    *y  = cmos_read(0x09);
}

void rtc_read(struct rtc_time *t) {
    uint8_t s, m, h, d, mo, y;
    uint8_t ls, lm, lh, ld, lmo, ly;

    read_raw(&s, &m, &h, &d, &mo, &y);
    do {                                    /* read until two reads agree */
        ls = s; lm = m; lh = h; ld = d; lmo = mo; ly = y;
        read_raw(&s, &m, &h, &d, &mo, &y);
    } while (s != ls || m != lm || h != lh || d != ld || mo != lmo || y != ly);

    uint8_t regb   = cmos_read(0x0B);
    int is_bcd     = !(regb & 0x04);
    int is_12h     = !(regb & 0x02);
    int pm         = h & 0x80;
    h &= 0x7F;

    if (is_bcd) {
        s = bcd2bin(s); m = bcd2bin(m); h = bcd2bin(h);
        d = bcd2bin(d); mo = bcd2bin(mo); y = bcd2bin(y);
    }
    if (is_12h) {
        if (h == 12) h = 0;
        if (pm) h = (uint8_t)(h + 12);
    }

    t->second = s;
    t->minute = m;
    t->hour   = h;
    t->day    = d;
    t->month  = mo;
    t->year   = (uint16_t)(2000 + y);       /* 2-digit year -> 21st century */
}
