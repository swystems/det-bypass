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

inline int set_packet_payload (char *buf, struct pingpong_payload *payload)
{
    struct iphdr *ip = (struct iphdr *) (buf + sizeof (struct ethhdr));
    ip->id = htons (payload->id);

    struct pingpong_payload *payload_ptr = (struct pingpong_payload *) (ip + 1);
    *payload_ptr = *payload;

    return 0;
}

struct sockaddr_ll build_sockaddr (int ifindex, const unsigned char *dest_mac)
{
    struct sockaddr_ll sock_addr;
    sock_addr.sll_ifindex = ifindex;
    sock_addr.sll_halen = ETH_ALEN;
    for (int i = 0; i < ETH_ALEN; ++i)
        sock_addr.sll_addr[i] = dest_mac[i];

    return sock_addr;
}

int send_pingpong_packet (int sock, const char *buf, struct sockaddr_ll *sock_addr)
{
    int ret = sendto (sock, buf, PACKET_SIZE, 0, (struct sockaddr *) sock_addr, sizeof (struct sockaddr_ll));
    if (ret < 0)
    {
        PERROR ("sendto");
        return -1;
    }

    return 0;
}

struct sender_data {
    int sock;
    uint32_t iters;
    uint64_t interval;
    char *base_packet;
    struct sockaddr_ll *sock_addr;
};

void *thread_send_packets (void *args)
{
    struct sender_data *data = (struct sender_data *) args;

    if (data->base_packet == NULL || data->sock_addr == NULL)
    {
        LOG (stderr, "ERR: base_packet or sock_addr is NULL\n");
        return NULL;
    }

    for (uint64_t id = 1; id <= data->iters; ++id)
    {
        struct pingpong_payload payload;
        payload.id = id;
        payload.phase = 0;
        payload.ts[0] = get_time_ns ();

        set_packet_payload (data->base_packet, &payload);

        int ret = send_pingpong_packet (data->sock, data->base_packet, data->sock_addr);
        if (ret < 0)
        {
            PERROR ("send_pingpong_packet");
            return NULL;
        }

        pp_sleep (data->interval);
    }

    return NULL;
}

static pthread_t sender_thread;

int start_sending_packets (int sock, uint32_t iters, uint64_t interval, char *base_packet, struct sockaddr_ll *sock_addr)
{
    struct sender_data *data = malloc (sizeof (struct sender_data));
    if (!data)
    {
        PERROR ("malloc");
        return -1;
    }

    data->sock = sock;
    data->iters = iters;
    data->interval = interval;
    data->base_packet = malloc (PACKET_SIZE);
    data->sock_addr = malloc (sizeof (struct sockaddr_ll));
    memcpy (data->base_packet, base_packet, PACKET_SIZE);
    memcpy (data->sock_addr, sock_addr, sizeof (struct sockaddr_ll));

    LOG (stdout, "Data copied!");

    int ret = pthread_create (&sender_thread, NULL, thread_send_packets, data);
    if (ret < 0)
    {
        PERROR ("pthread_create");
        return -1;
    }

    return 0;
}