#include "utils.h"

inline uint64_t get_time_ns (void)
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

inline void pp_sleep (uint64_t ns)
{
    uint32_t s = ns / 1000000000LL;

    struct timespec t;
    t.tv_sec = s;
    t.tv_nsec = ns - s * 1000000000LL;
    clock_nanosleep (CLOCK_MONOTONIC, 0, &t, NULL);
}