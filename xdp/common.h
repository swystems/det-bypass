#pragma once

#include <linux/types.h>

#define PACKET_SIZE 1024

struct pingpong_payload {
    __u16 phase;
    __u16 id;
    __u64 ts[4];
};