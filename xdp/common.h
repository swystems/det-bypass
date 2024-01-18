#pragma once

#include <linux/types.h>

#define BUSY_WAIT(condition)                 \
    do                                       \
    {                                        \
        while (condition)                    \
        {                                    \
            __asm__ __volatile__ ("pause;"); \
        }                                    \
    } while (0)

#ifndef DEBUG
#define DEBUG 0
#endif

#if DEBUG
#define LOG(stream, fmt, ...)                 \
    do                                        \
    {                                         \
        fprintf (stream, fmt, ##__VA_ARGS__); \
        fprintf (stream, "\n");               \
        fflush (stream);                      \
    } while (0)

#define PERROR(errno)   \
    do                  \
    {                   \
        perror (errno); \
    } while (0)
#else
#define LOG(stream, fmt, ...)
#define PERROR(errno)
#endif

#define PACKET_SIZE 1024
#define ETH_P_PINGPONG 0x2002

struct pingpong_payload {
    __u32 phase;
    __u32 id;
    __u64 ts[4];
};
