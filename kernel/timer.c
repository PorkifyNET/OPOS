#include <stdbool.h>
#include <stdint.h>

#include "interrupts.h"
#include "io.h"
#include "scheduler.h"
#include "svga.h"
#include "timer.h"

#define PIT_INPUT_HZ 1193182
#define PIT_FREQUENCY_HZ 100
#define PIT_DIVISOR (PIT_INPUT_HZ / PIT_FREQUENCY_HZ)
#define NANOSECONDS_PER_SECOND 1000000000ULL
static volatile uint64_t ticks;

void timer_initialize(void) {
    io_out8(0x43, 0x36);
    io_out8(0x40, PIT_DIVISOR & 0xff);
    io_out8(0x40, (PIT_DIVISOR >> 8) & 0xff);
    interrupts_unmask_irq(0);
}

void timer_irq_handler(void) {
    ticks++;
    svga_timer_tick();
}

uint64_t timer_ticks(void) {
    return ticks;
}

static bool sleep_ticks(uint64_t sleep_ticks) {
    uint64_t start_ticks = timer_ticks();
    if (sleep_ticks > UINT64_MAX - start_ticks) {
        return false;
    }
    return scheduler_sleep_until(start_ticks + sleep_ticks);
}

/* Ticks covering at least the duration. A tick is PIT_DIVISOR input
   cycles (9.99932 ms), slightly under 10 ms, so a duration counted in
   nominal 10 ms ticks would come up short on the monotonic clock. */
bool timer_duration_ticks(uint64_t seconds, uint64_t nanoseconds,
                          uint64_t *ticks_out) {
    if (nanoseconds >= NANOSECONDS_PER_SECOND) {
        return false;
    }

    uint64_t whole_ticks_per_second = PIT_INPUT_HZ / PIT_DIVISOR;
    uint64_t remainder_per_second = PIT_INPUT_HZ % PIT_DIVISOR;
    if (seconds > UINT64_MAX / whole_ticks_per_second
        || (remainder_per_second != 0
            && seconds > UINT64_MAX / remainder_per_second)) {
        return false;
    }

    uint64_t whole_ticks = seconds * whole_ticks_per_second;
    uint64_t remainder = seconds * remainder_per_second;
    uint64_t additional_ticks = remainder / PIT_DIVISOR;
    remainder %= PIT_DIVISOR;
    if (additional_ticks > UINT64_MAX - whole_ticks) {
        return false;
    }
    whole_ticks += additional_ticks;

    uint64_t fractional_numerator =
        remainder * NANOSECONDS_PER_SECOND + nanoseconds * PIT_INPUT_HZ;
    uint64_t fractional_denominator =
        (uint64_t)PIT_DIVISOR * NANOSECONDS_PER_SECOND;
    uint64_t fractional_ticks =
        (fractional_numerator + fractional_denominator - 1)
            / fractional_denominator;
    if (fractional_ticks > UINT64_MAX - whole_ticks) {
        return false;
    }

    *ticks_out = whole_ticks + fractional_ticks;
    return true;
}

bool timer_sleep_duration(uint64_t seconds, uint64_t nanoseconds) {
    uint64_t ticks_needed;
    if (!timer_duration_ticks(seconds, nanoseconds, &ticks_needed)) {
        return false;
    }
    return sleep_ticks(ticks_needed);
}

void timer_ticks_duration(uint64_t elapsed_ticks, uint64_t *seconds,
                          uint64_t *nanoseconds) {
    uint64_t whole_tick_groups = elapsed_ticks / PIT_INPUT_HZ;
    uint64_t remaining_ticks = elapsed_ticks % PIT_INPUT_HZ;
    uint64_t fractional_cycles = remaining_ticks * PIT_DIVISOR;

    *seconds = whole_tick_groups * PIT_DIVISOR
        + fractional_cycles / PIT_INPUT_HZ;
    *nanoseconds = (fractional_cycles % PIT_INPUT_HZ) * 1000000000ULL
        / PIT_INPUT_HZ;
}

void timer_monotonic_time(uint64_t *seconds, uint64_t *nanoseconds) {
    timer_ticks_duration(timer_ticks(), seconds, nanoseconds);
}

uint64_t timer_monotonic_resolution_nanoseconds(void) {
    uint64_t period = (uint64_t)PIT_DIVISOR * 1000000000ULL;
    return (period + PIT_INPUT_HZ - 1) / PIT_INPUT_HZ;
}
