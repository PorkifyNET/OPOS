/* OPOS SDK C library: clocks and calendar time (see include/time.h). */
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include <opos.h>

int clock_gettime(clockid_t clock, struct timespec *result) {
    struct opos_timespec time;
    long status = opos_syscall2(228, clock, (long)&time);
    if (status != 0) {
        return -1;
    }
    result->tv_sec = time.seconds;
    result->tv_nsec = time.nanoseconds;
    return 0;
}

time_t time(time_t *result) {
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_REALTIME, &now);
    if (result != NULL) {
        *result = now.tv_sec;
    }
    return now.tv_sec;
}

int gettimeofday(struct timeval *result, void *zone) {
    (void)zone;
    struct timespec now = {0, 0};
    int status = clock_gettime(CLOCK_REALTIME, &now);
    result->tv_sec = now.tv_sec;
    result->tv_usec = now.tv_nsec / 1000;
    return status;
}

clock_t clock(void) {
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * CLOCKS_PER_SEC + now.tv_nsec / 1000;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (month 1-12). */
static long days_from_civil(long year, int month, int day) {
    year -= month <= 2;
    long era = (year >= 0 ? year : year - 399) / 400;
    long year_of_era = year - era * 400;
    long day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
        + day - 1;
    long day_of_era = year_of_era * 365 + year_of_era / 4
        - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

struct tm *gmtime_r(const time_t *time, struct tm *result) {
    long seconds = *time;
    long days = seconds / 86400;
    long rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        days--;
    }
    result->tm_hour = (int)(rest / 3600);
    result->tm_min = (int)(rest / 60 % 60);
    result->tm_sec = (int)(rest % 60);
    result->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01: Thursday */

    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long day_of_era = z - era * 146097;
    long year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524
                        - day_of_era / 146096) / 365;
    long year = year_of_era + era * 400;
    long day_of_year = day_of_era
        - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    long mp = (5 * day_of_year + 2) / 153;
    int day = (int)(day_of_year - (153 * mp + 2) / 5 + 1);
    int month = (int)(mp < 10 ? mp + 3 : mp - 9);
    year += month <= 2;

    result->tm_year = (int)(year - 1900);
    result->tm_mon = month - 1;
    result->tm_mday = day;
    result->tm_yday = (int)(days - days_from_civil(year, 1, 1));
    result->tm_isdst = 0;
    result->tm_gmtoff = 0;
    result->tm_zone = "UTC";
    return result;
}

struct tm *gmtime(const time_t *time) {
    static struct tm result;
    return gmtime_r(time, &result);
}

struct tm *localtime_r(const time_t *time, struct tm *result) {
    return gmtime_r(time, result);
}

struct tm *localtime(const time_t *time) {
    static struct tm result;
    return gmtime_r(time, &result);
}

time_t timegm(struct tm *tm) {
    long year = 1900L + tm->tm_year;
    long month = tm->tm_mon;
    year += month / 12;
    month %= 12;
    if (month < 0) {
        month += 12;
        year--;
    }
    time_t result = (time_t)(days_from_civil(year, (int)month + 1, 1)
                             + tm->tm_mday - 1) * 86400
        + (time_t)tm->tm_hour * 3600 + (time_t)tm->tm_min * 60
        + tm->tm_sec;
    gmtime_r(&result, tm);
    return result;
}

time_t mktime(struct tm *tm) {
    return timegm(tm);
}
