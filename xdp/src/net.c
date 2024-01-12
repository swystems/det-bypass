#include "net.h"
int convert_ip (const char *ip, uint32_t *ip_addr)
{
    struct in_addr ip_addr_struct;
    if (inet_aton (ip, &ip_addr_struct) == 0)
    {
        PERROR ("inet_aton");
        return -1;
    }
    *ip_addr = ip_addr_struct.s_addr;
    return 0;
}

int build_base_packet (char *buf, const uint8_t *src_mac, const uint8_t *dest_mac,
                       const char *src_ip, const char *dest_ip)
{
    struct ethhdr *eth = (struct ethhdr *) buf;
    for (int i = 0; i < ETH_ALEN; ++i)
    {
        eth->h_source[i] = src_mac[i];
        eth->h_dest[i] = dest_mac[i];
    }

    eth->h_proto = __constant_htons (ETH_P_PINGPONG);

    uint32_t src_ip_addr;
    if (convert_ip (src_ip, &src_ip_addr) < 0)
        return -1;

    uint32_t dest_ip_addr;
    if (convert_ip (dest_ip, &dest_ip_addr) < 0)
        return -1;

    struct iphdr *ip = (struct iphdr *) (eth + 1);
    ip->ihl = 5;
    ip->version = 4;
    ip->tos = 0;
    ip->tot_len = htons (PACKET_SIZE - sizeof (struct ethhdr));
    ip->id = 0;
    ip->frag_off = htons (0);
    ip->ttl = 64;
    ip->protocol = IPPROTO_RAW;
    ip->check = 0;
    ip->saddr = src_ip_addr;
    ip->daddr = dest_ip_addr;

    return 0;
}

int setup_packet_payload (char *buf, uint32_t id)
{
    struct iphdr *ip = (struct iphdr *) (buf + sizeof (struct ethhdr));
    ip->id = htons (id);

    struct pingpong_payload *payload = (struct pingpong_payload *) (ip + 1);
    payload->id = id;
    payload->phase = 0;
    payload->ts[0] = get_time_ns ();

    return 0;
}

struct sockaddr_ll build_sockaddr (int ifindex, const char *dest_mac)
{
    struct sockaddr_ll sock_addr;
    sock_addr.sll_ifindex = ifindex;
    sock_addr.sll_halen = ETH_ALEN;
    for (int i = 0; i < ETH_ALEN; ++i)
        sock_addr.sll_addr[i] = dest_mac[i];

    return sock_addr;
}

void send_packets (int ifindex, const char *server_ip, uint64_t iters, uint64_t interval)
{
    const uint8_t src_mac[ETH_ALEN] = {0x9c, 0xdc, 0x71, 0x5d, 0x51, 0x31};
    const uint8_t dest_mac[ETH_ALEN] = {0x9c, 0xdc, 0x71, 0x5d, 0xd5, 0xf1};

    const char *src_ip = "10.10.1.2";

    int sock = socket (AF_PACKET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0)
    {
        perror ("socket");
        return;
    }

    char buf[PACKET_SIZE];
    memset (buf, 0, PACKET_SIZE);

    int ret = build_base_packet (buf, src_mac, dest_mac, src_ip, server_ip);
    if (ret < 0)
        return;

    struct sockaddr_ll sock_addr = build_sockaddr (ifindex, (const char *) dest_mac);

    for (uint64_t i = 0; i < iters; ++i)
    {
        setup_packet_payload (buf, i);

        int ret = sendto (sock, buf, PACKET_SIZE, 0, (struct sockaddr *) &sock_addr, sizeof (struct sockaddr_ll));
        if (ret < 0)
        {
            perror ("sendto");
            return;
        }

        long long start = get_time_ns ();
        BUSY_WAIT (get_time_ns () - start < interval);
    }

    LOG (stdout, "Sent %llu packets\n", iters - 1);

    close (sock);
}
