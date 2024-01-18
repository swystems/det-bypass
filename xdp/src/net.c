#include "net.h"

int setup_socket (void)
{
    int sock = socket (AF_PACKET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0)
    {
        perror ("socket");
        return sock;
    }

    return sock;
}

int convert_ip (const char *ip, uint32_t *ip_addr)
{
    if (ip == NULL)
    {
        *ip_addr = 0;
        return 0;
    }

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

int set_packet_payload (char *buf, struct pingpong_payload *payload)
{
    struct iphdr *ip = (struct iphdr *) (buf + sizeof (struct ethhdr));
    ip->id = htons (payload->id);

    struct pingpong_payload *payload_ptr = (struct pingpong_payload *) (ip + 1);
    *payload_ptr = *payload;

    return 0;
}

/**
 * Build a sockaddr_ll structure with the given ifindex and destination mac address.
 * This structure is used to send packets to the remote node.
 *
 * @param ifindex the interface index
 * @param dest_mac the destination mac address
 * @return the constructed sockaddr_ll structure
 */
struct sockaddr_ll build_sockaddr (int ifindex, const unsigned char *dest_mac)
{
    struct sockaddr_ll sock_addr;
    sock_addr.sll_ifindex = ifindex;
    sock_addr.sll_halen = ETH_ALEN;
    for (int i = 0; i < ETH_ALEN; ++i)
        sock_addr.sll_addr[i] = dest_mac[i];

    return sock_addr;
}

int send_pingpong_packet (int sock, const char *buf, int ifindex, const uint8_t *dest_mac)
{
    struct sockaddr_ll sock_addr = build_sockaddr (ifindex, (const unsigned char *) dest_mac);

    int ret = sendto (sock, buf, PACKET_SIZE, 0, (struct sockaddr *) &sock_addr, sizeof (sock_addr));
    if (ret < 0)
    {
        PERROR ("sendto");
        return -1;
    }

    return 0;
}