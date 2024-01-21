#include "common.h"
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>

struct {
    __uint (type, BPF_MAP_TYPE_ARRAY);
    __uint (map_flags, BPF_F_MMAPABLE);
    __type (key, __u32);
    __type (value, struct pingpong_payload);
    __uint (max_entries, 1);
    __uint (pinning, LIBBPF_PIN_BY_NAME);
} last_payload
    SEC (".maps");

SEC ("xdp")
int xdp_main (struct xdp_md *ctx)
{
    void *data_start = (void *) (long) ctx->data;
    void *data_end = (void *) (long) ctx->data_end;

    // custom packet: Ethernet + IP + pingpong_payload; size is always PACKET_SIZE bytes (defined in common.h)
    // what identifies the pingpong packet is the custom ETH type ETH_P_PINGPONG (defined in common.h)
    if (data_start + sizeof (struct ethhdr) + sizeof (struct iphdr) + sizeof (struct pingpong_payload) > data_end)
    {
        return XDP_PASS;
    }

    struct ethhdr *eth = data_start;
    if (eth->h_proto != __constant_htons (ETH_P_PINGPONG))
    {
        return XDP_PASS;
    }

    struct pingpong_payload *payload = data_start + sizeof (struct ethhdr) + sizeof (struct iphdr);

    __u32 key = 0;
    bpf_map_update_elem (&last_payload, &key, payload, BPF_ANY);

    return XDP_DROP;
}

char _license[] SEC ("license") = "GPL";