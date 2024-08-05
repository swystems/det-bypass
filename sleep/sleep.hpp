#include <cstdint>



#define BARRIER() __asm__ __volatile__ ("" ::: "memory")



/**
 * Retrieve the current time in nanoseconds.
 * @return The current time in nanoseconds.
 */
uint64_t get_time_ns (void);

void pp_sleep (uint64_t ns, double err);

uint64_t get_threshold(uint64_t interval, uint64_t err);
