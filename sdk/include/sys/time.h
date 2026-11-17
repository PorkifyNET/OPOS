/* OPOS SDK C library: gettimeofday. */
#ifndef OPOS_SYS_TIME_H
#define OPOS_SYS_TIME_H

#include <time.h>

typedef long suseconds_t;

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

/* The time zone argument is ignored. */
int gettimeofday(struct timeval *result, void *zone);

#endif
