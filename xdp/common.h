#pragma once

#include <linux/types.h>

#define PACKET_SIZE 1024

struct pingpong_payload {
    __u32 phase;
    __u32 id;
    __u64 ts[4];
};