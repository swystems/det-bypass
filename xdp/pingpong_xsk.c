#include "common.h"
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>

struct {
    __uint (type, BPF_MAP_TYPE_XSKMAP);
    __type (key, int);
    __type (value, int);
    __uint (max_entries, 1);
    __uint (pinning, LIBBPF_PIN_BY_NAME);
} xsk_map
    SEC (".maps");

SEC ("xdp")
int xdp_xsk (struct xdp_md *ctx)
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

    if (payload->magic != PINGPONG_MAGIC)
    {
        bpf_printk ("Invalid magic number: %u\n", payload->magic);
        return XDP_PASS;
    }

    return bpf_redirect_map (&xsk_map, 0, XDP_DROP);
}

char _license[] SEC ("license") = "GPL";