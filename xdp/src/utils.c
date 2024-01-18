#include "utils.h"



inline uint64_t get_time_ns (void)
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

inline void pp_sleep (uint64_t ns)
{
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = ns;
    clock_nanosleep(CLOCK_MONOTONIC, 0, &t, NULL);
}