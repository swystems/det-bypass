#pragma once

#include <stdint.h>
#include <time.h>

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) > (b) ? (b) : (a))

/**
 * Retrieve the current time in nanoseconds.
 * @return The current time in nanoseconds.
 */
uint64_t get_time_ns (void);

void pp_sleep (uint64_t ns);