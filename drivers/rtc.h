/* ===========================================================================
 * PumpkinOS - CMOS real-time clock
 * ========================================================================= */
#ifndef PUMPKIN_RTC_H
#define PUMPKIN_RTC_H

#include <stdint.h>

struct rtc_time {
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
};

/* Read the current wall-clock time from the CMOS RTC. */
void rtc_read(struct rtc_time *t);

#endif /* PUMPKIN_RTC_H */
