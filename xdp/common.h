#pragma once

#define PACKET_SIZE 2048

struct pingpong_payload {
    __u16 phase;
    __u16 id;
    __u64 ts[4];
};