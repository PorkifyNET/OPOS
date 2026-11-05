#ifndef OPOS_TIMER_H
#define OPOS_TIMER_H

#include <stdbool.h>
#include <stdint.h>

void timer_initialize(void);
void timer_irq_handler(void);
uint64_t timer_ticks(void);
bool timer_sleep_duration(uint64_t seconds, uint64_t nanoseconds);
void timer_monotonic_time(uint64_t *seconds, uint64_t *nanoseconds);
uint64_t timer_monotonic_resolution_nanoseconds(void);

#endif
