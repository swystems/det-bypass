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

struct lock_map_element {
    struct bpf_spin_lock value;
    __u32 index;
};

struct {
    __uint (type, BPF_MAP_TYPE_ARRAY);
    __type (key, __u32);
    __type (value, struct lock_map_element);
    __uint (max_entries, 1);
} lck SEC (".maps");

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
    if (!payload)
    {
        bpf_printk ("Invalid payload\n");
        return -1;
    }

    __u32 lock_key = 0;
    struct lock_map_element *lock = bpf_map_lookup_elem (&lck, &lock_key);
    if (!lock)
    {
        bpf_printk ("Failed to lookup lock element\n");
        return -1;
    }

    bpf_spin_lock (&lock->value);
    __u32 key = lock->index;
    lock->index = (lock->index + 1) % PACKETS_MAP_SIZE;
    bpf_spin_unlock (&lock->value);

    struct pingpong_payload *old_payload = bpf_map_lookup_elem (&last_payload, &key);
    if (!old_payload)
    {
        bpf_printk ("Failed to lookup element at index: %D\n", key);
        return -1;
    }

    if (valid_pingpong_payload (old_payload))
    {
        /*
         * If there is already a packet at the current index, it means that the map is full.
         * Drop the packet and reset the next index to the current one.
         *
         * TODO: Fix the data race in the reset of the index
         */
//        bpf_spin_lock (&lock->value);
//        lock->index = key;
//        bpf_spin_unlock (&lock->value);
        return -1;
    }

    bpf_printk ("Adding packet id: %u at index: %u\n", payload->id, key);

    *old_payload = *payload;

    return 0;
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
        bpf_printk ("Packet is too small\n");
        return XDP_PASS;
    }

    struct ethhdr *eth = data_start;
    if (eth->h_proto != __constant_htons (ETH_P_PINGPONG))
    {
        bpf_printk ("Invalid eth protocol: %u\n", eth->h_proto);
        return XDP_PASS;
    }

    struct pingpong_payload *payload = data_start + sizeof (struct ethhdr) + sizeof (struct iphdr);

    if (!valid_pingpong_payload (payload))
    {
        bpf_printk ("Invalid pingpong payload.\n");
        return XDP_PASS;
    }

    add_packet_to_map (payload);

    return XDP_DROP;
}

char _license[] SEC ("license") = "GPL";