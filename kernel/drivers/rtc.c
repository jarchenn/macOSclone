/* CMOS real-time clock: read once at boot for CLOCK_REALTIME (0.6). The
 * RTC is assumed to hold UTC (QEMU's default `-rtc base=utc`). */
#include "types.h"
#include "arch/x86_64/io.h"
#include "core/log.h"

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

static int64_t days_from_civil(int64_t y, int m, int d)
{
    /* Howard Hinnant's algorithm: days since 1970-01-01. */
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t rtc_read_epoch(void);
int64_t rtc_read_epoch(void)
{
    int sec, min, hour, day, mon, year, again;
    /* Read until two consecutive reads agree and no update is running. */
    for (int tries = 0; tries < 1000; tries++) {
        while (cmos(0x0A) & 0x80)
            cpu_pause();
        sec = cmos(0x00); min = cmos(0x02); hour = cmos(0x04);
        day = cmos(0x07); mon = cmos(0x08); year = cmos(0x09);
        again = cmos(0x00);
        if (again == sec)
            break;
    }
    uint8_t b = cmos(0x0B);
    bool pm = hour & 0x80;
    hour &= 0x7F;
    if (!(b & 0x04)) {
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
    }
    if (!(b & 0x02)) { /* 12-hour clock */
        hour %= 12;
        if (pm)
            hour += 12;
    }
    int64_t y = 2000 + year;
    if (mon < 1 || mon > 12 || day < 1 || day > 31) {
        klog("rtc: implausible date, using the epoch");
        return 0;
    }
    int64_t t = days_from_civil(y, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
    klog("rtc: %04lld-%02d-%02d %02d:%02d:%02d UTC", (long long)y, mon, day, hour, min, sec);
    return t;
}
