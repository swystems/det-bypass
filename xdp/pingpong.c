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
    __uint (max_entries, PACKETS_MAP_SIZE);
    __uint (pinning, LIBBPF_PIN_BY_NAME);
} last_payload
    SEC (".maps");

static __u32 current_item_idx = 0;

/**
 * Add the given payload to the map.
 *
 * The packets are pushed to the map in a circular fashion.
 * The only way for a packet to be overwritten is if the map gets full, which is very unlikely
 * if the user-space continuously polls and the map capacity is big enough.
 *
 * @param payload the payload to add
 * @return 0 on success, -1 on failure
 */
int add_packet_to_map (struct pingpong_payload *payload)
{
    current_item_idx++;
    __u32 key = current_item_idx % PACKETS_MAP_SIZE;
    return bpf_map_update_elem (&last_payload, &key, payload, BPF_ANY);
}

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

    if (payload->magic != PINGPONG_MAGIC)
    {
        bpf_printk ("Invalid magic number: %u\n", payload->magic);
        return XDP_PASS;
    }

    bpf_printk ("Received packet with id %u\n", payload->id);

    add_packet_to_map (payload);

    return XDP_DROP;
}

char _license[] SEC ("license") = "GPL";