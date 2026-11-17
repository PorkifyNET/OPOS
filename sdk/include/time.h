/*
 * OPOS SDK C library: calendar time. OPOS keeps the clock in UTC and has
 * no time zones yet, so local time is UTC (tm_gmtoff is 0).
 */
#ifndef OPOS_TIME_H
#define OPOS_TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;
typedef int clockid_t;

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCKS_PER_SEC 1000000L

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;   /* 0-11 */
    int tm_year;  /* years since 1900 */
    int tm_wday;  /* 0 = Sunday */
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

time_t time(time_t *result);
int clock_gettime(clockid_t clock, struct timespec *result);
clock_t clock(void);
struct tm *gmtime(const time_t *time);
struct tm *gmtime_r(const time_t *time, struct tm *result);
struct tm *localtime(const time_t *time);
struct tm *localtime_r(const time_t *time, struct tm *result);
/* Reads the fields as local time (UTC) and normalises them. */
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);

#endif
