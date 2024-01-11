#include "common.h"
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>

struct {
    __uint (type, BPF_MAP_TYPE_ARRAY);
    __uint (map_flags, BPF_F_MMAPABLE);
    __type (key, __u32);
    __type (value, struct pingpong_payload);
    __uint (max_entries, 1);
} last_timestamp
    SEC (".maps");

SEC ("xdp")
int xdp_main (struct xdp_md *ctx)
{
    __u64 receive_ts = bpf_ktime_get_ns ();
    void *data_start = (void *) (long) ctx->data;
    void *data_end = (void *) (long) ctx->data_end;

    // custom packet: Ethernet + IP + pingpong_payload; size is always PACKET_SIZE bytes (defined in common.h)
    // what identifies the pingpong packet is the custom ETH type 0x2002
    if (data_start + sizeof (struct ethhdr) + sizeof (struct iphdr) + sizeof (struct pingpong_payload) > data_end)
    {
        return XDP_PASS;
    }

    struct ethhdr *eth = data_start;
    if (eth->h_proto != __constant_htons (0x2002))
    {
        return XDP_PASS;
    }

    struct pingpong_payload *payload = data_start + sizeof (struct ethhdr) + sizeof (struct iphdr);
    __u8 phase = payload->phase;

    /**
     * Phase 0: I am the server and just received a packet sent from the client
     * Phase 1: I am the client and just received a packet sent from the server
     */
    if (phase == 0)
    {
        // Server
        payload->ts[1] = receive_ts;

        // swap source and destination MAC addresses
        __u8 tmp[ETH_ALEN];
        __builtin_memcpy (tmp, eth->h_source, ETH_ALEN);
        __builtin_memcpy (eth->h_source, eth->h_dest, ETH_ALEN);
        __builtin_memcpy (eth->h_dest, tmp, ETH_ALEN);

        payload->phase = !phase;
        payload->ts[2] = bpf_ktime_get_ns ();

        return XDP_TX;
    }
    else if (phase == 1)
    {
        // Client
        payload->ts[3] = receive_ts;

        // store in the map
        __u32 key = 0;
        bpf_map_update_elem (&last_timestamp, &key, payload, BPF_ANY);

        return XDP_DROP;// done with this packet
    }

    return XDP_PASS;
}

char _license[] SEC ("license") = "GPL";