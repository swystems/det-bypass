#pragma once

#include <linux/types.h>

#define LIKELY(x) (__builtin_expect (!!(x), 1))
#define UNLIKELY(x) (__builtin_expect (!!(x), 0))

#define BARRIER() __asm__ __volatile__ ("" ::: "memory")

#define BUSY_WAIT(cond) \
    do                  \
    {                   \
        BARRIER ();     \
    } while (cond)

#ifndef DEBUG
#define DEBUG 0
#endif

#if DEBUG
#define LOG(stream, fmt, ...)                 \
    do                                        \
    {                                         \
        fprintf (stream, fmt, ##__VA_ARGS__); \
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

#define TIME_CODE(code)                      \
    do                                       \
    {                                        \
        uint64_t start = get_time_ns ();     \
        code;                                \
        uint64_t end = get_time_ns ();       \
        printf ("Time: %lu\n", end - start); \
        fflush (stdout);                     \
    } while (0)
/**
 * Size of the whole pingpong packet.
 * Although the actual payload (Ethernet header + IP header + pingpong payload) is smaller than this,
 * the packet should be the same across all the XDP, RDMA and DPDK measurements.
 */
#define PACKET_SIZE 1024

/**
 * Size of the exchanged packet containing MAC address (6 bytes) and IP address (4 bytes) of each machine.
 * This packet is sent before the start of pingpong to exchange the addresses without hardcoding them
 */
#define INFO_PACKET_SIZE (ETH_ALEN + sizeof (uint32_t))

// Custom ethernet protocol number
#define ETH_P_PINGPONG 0x2002

struct pingpong_payload {
    __u32 id;
    __u32 phase;
    __u64 ts[4];
};
