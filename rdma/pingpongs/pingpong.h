/*
 * Copyright (c) 2006 Cisco Systems.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef IBV_PINGPONG_H
#define IBV_PINGPONG_H

#include <infiniband/verbs.h>

#define LOG(...)                         \
    do                                   \
    {                                    \
        printf ("%llu: ", get_nanos ()); \
        printf (__VA_ARGS__);            \
        printf ("\n");                   \
    } while (0)

struct pingpong_payload {
    /**
     * Timestamps at each stage of the pingpong to measure delay and jitter.
     * The first timestamp is set by the sender before sending the packet.
     * The second timestamp is set by the receiver when it receives the packet.
     * The third timestamp is set by the receiver when it sends the packet back.
     * The fourth timestamp is set by the sender when it receives the packet back.
     *
     * The timestamps are in nanoseconds.
     * It would be best if the NIC timestamps were available, but they are not on CloudLab.
     */
    uint64_t ts[4];
} __attribute__ ((packed));

struct pingpong_data {
    struct pingpong_payload *payloads;
    int num_payloads;
};

enum ibv_mtu
pp_mtu_to_enum (int mtu);
int pp_get_port_info (struct ibv_context *context, int port,
                      struct ibv_port_attr *attr);
void wire_gid_to_gid (const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid (const union ibv_gid *gid, char wgid[]);

struct pingpong_data *init_pingpong_data (int num_payloads);
void free_pingpong_data (struct pingpong_data *data);

void print_payload (struct pingpong_payload *payload);
void update_payload (struct pingpong_payload *payload, int stage);
void store_payload (struct pingpong_payload *payload, struct pingpong_data *data);
void save_payloads_to_file (struct pingpong_data *data, unsigned int warmup, const char *foldername);

long long get_nanos (void);

#endif /* IBV_PINGPONG_H */
