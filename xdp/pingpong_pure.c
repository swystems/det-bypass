#include "../common/common.h"
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <stdint.h>

#if SERVER
__attribute__((__always_inline__)) static inline __u16 csum_fold_helper(__u64 csum)
{
    int i;
#pragma unroll
    for (i = 0; i < 4; i++)
    {
        if (csum >> 16)
            csum = (csum & 0xffff) + (csum >> 16);
    }
    return ~csum;
}

__attribute__((__always_inline__)) static inline void ipv4_csum(void *data_start, int data_size, __u64 *csum)
{
    *csum = bpf_csum_diff(0, 0, data_start, data_size, *csum);
    *csum = csum_fold_helper(*csum);
}
#endif

SEC ("xdp")
int xdp_main (struct xdp_md *ctx)
{
    __u64 ts = bpf_ktime_get_ns ();
    void *data_end = (void *) (long) ctx->data_end;
    void *data = (void *) (long) ctx->data;

    if (data + sizeof (struct ethhdr) + sizeof (struct iphdr) + sizeof (struct udphdr) + sizeof (struct pingpong_payload) > data_end)
    {
        bpf_printk ("Packet too small\n");
        return XDP_PASS;
    }

    struct ethhdr *eth = data;
    struct iphdr *ip = data + sizeof (struct ethhdr);
    struct udphdr *udp = data + sizeof (struct ethhdr) + sizeof (struct iphdr);
    struct pingpong_payload *payload = data + sizeof (struct ethhdr) + sizeof (struct iphdr) + sizeof (struct udphdr);

    if (eth->h_proto != bpf_htons (ETH_P_IP) || ip->protocol != IPPROTO_UDP || udp->dest != bpf_htons (XDP_UDP_PORT))
    {
        bpf_printk ("Invalid packet\n");
        return XDP_PASS;
    }

    if (!valid_pingpong_payload(payload))
    {
        bpf_printk("Invalid payload\n");
        return XDP_PASS;
    }

#if !SERVER
    payload->ts[3] = ts;
    return XDP_PASS;
#else
    payload->ts[1] = ts;

    // Swap the MAC addresses
    unsigned char tmp[ETH_ALEN];
    __builtin_memcpy (tmp, eth->h_dest, ETH_ALEN);
    __builtin_memcpy (eth->h_dest, eth->h_source, ETH_ALEN);
    __builtin_memcpy (eth->h_source, tmp, ETH_ALEN);

    // Swap the IP addresses
    __u32 tmp_ip = ip->daddr;
    ip->daddr = ip->saddr;
    ip->saddr = tmp_ip;
    ip->check = 0;
    __u64 csum = 0;
    ipv4_csum(ip, sizeof(struct iphdr), &csum);
    ip->check = csum;
    ip->ttl = 64;

    udp->source = bpf_htons (XDP_UDP_PORT);
    udp->dest = bpf_htons (XDP_UDP_PORT);
    udp->check = 0;

    payload->ts[2] = bpf_ktime_get_ns ();
    // Send the packet back
    return XDP_TX;
#endif
}

char _license[] SEC ("license") = "GPL";
