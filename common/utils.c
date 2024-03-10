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

void hex_dump (const void *data, size_t size)
{
    // source: https://gist.github.com/ccbrown/9722406
    char ascii[17];
    size_t i, j;
    ascii[16] = '\0';
    for (i = 0; i < size; ++i)
    {
        printf ("%02X ", ((unsigned char *) data)[i]);
        if (((unsigned char *) data)[i] >= ' ' && ((unsigned char *) data)[i] <= '~')
        {
            ascii[i % 16] = ((unsigned char *) data)[i];
        }
        else
        {
            ascii[i % 16] = '.';
        }
        if ((i + 1) % 8 == 0 || i + 1 == size)
        {
            printf (" ");
            if ((i + 1) % 16 == 0)
            {
                printf ("|  %s \n", ascii);
            }
            else if (i + 1 == size)
            {
                ascii[(i + 1) % 16] = '\0';
                if ((i + 1) % 16 <= 8)
                {
                    printf (" ");
                }
                for (j = (i + 1) % 16; j < 16; ++j)
                {
                    printf ("   ");
                }
                printf ("|  %s \n", ascii);
            }
        }
    }
}

