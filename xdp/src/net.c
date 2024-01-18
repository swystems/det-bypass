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

int retrieve_local_ip (int ifindex, uint32_t *out_addr)
{
    char ifname[IF_NAMESIZE];
    if (if_indextoname (ifindex, ifname) == NULL)
    {
        PERROR ("if_indextoname");
        return -1;
    }

    int sock = socket (AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        PERROR ("socket");
        return -1;
    }

    struct ifreq ifr;
    memset (&ifr, 0, sizeof (struct ifreq));
    strncpy (ifr.ifr_name, ifname, IF_NAMESIZE);

    int ret = ioctl (sock, SIOCGIFADDR, &ifr);
    if (ret < 0)
    {
        PERROR ("ioctl");
        return -1;
    }

    close (sock);

    struct sockaddr_in *addr = (struct sockaddr_in *) &ifr.ifr_addr;
    *out_addr = addr->sin_addr.s_addr;
    return 0;
}

int retrieve_local_mac (int ifindex, uint8_t *out_mac)
{
    char ifname[IF_NAMESIZE + 1];
    if (if_indextoname (ifindex, ifname) == NULL)
    {
        PERROR ("if_indextoname");
        return -1;
    }

    // read /sys/class/net/<ifname>/address
    char path[192];
    snprintf (path, 192, "/sys/class/net/%s/address", ifname);
    FILE *f = fopen (path, "r");
    if (!f)
    {
        PERROR ("fopen");
        return -1;
    }

    char mac_str[18];
    if (fgets (mac_str, 18, f) == NULL)
    {
        PERROR ("fgets");
        return -1;
    }

    if (fclose (f) != 0)
    {
        PERROR ("fclose");
        return -1;
    }

    int ret = sscanf (mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &out_mac[0], &out_mac[1], &out_mac[2], &out_mac[3], &out_mac[4], &out_mac[5]);
    if (ret != 6)
    {
        PERROR ("sscanf");
        return -1;
    }

    return 0;
}

int exchange_addresses (const int ifindex, const char *server_ip, bool is_server,
                        uint8_t *src_mac, uint8_t *dest_mac,
                        uint32_t *src_ip, uint32_t *dest_ip)
{
    LOG (stdout, "Starting exchange of addresses...\n");
    fflush (stdout);
    retrieve_local_mac (ifindex, src_mac);
    retrieve_local_ip (ifindex, src_ip);

    int sock = socket (AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror ("socket");
        return -1;
    }

    // bind to local address port 1234
    struct sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons (1234);
    local_addr.sin_addr.s_addr = INADDR_ANY;
    int ret = bind (sock, (struct sockaddr *) &local_addr, sizeof (struct sockaddr_in));
    if (ret < 0)
    {
        perror ("bind");
        return -1;
    }

    if (!is_server)
    {
        // send my info to the server
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons (1234);
        server_addr.sin_addr.s_addr = inet_addr (server_ip);

        char buf[INFO_PACKET_SIZE];
        memcpy (buf, src_mac, ETH_ALEN);
        memcpy (buf + ETH_ALEN, src_ip, sizeof (uint32_t));
        int ret = sendto (sock, buf, INFO_PACKET_SIZE, 0, (struct sockaddr *) &server_addr, sizeof (struct sockaddr_in));
        if (ret < 0)
        {
            perror ("sendto");
            return -1;
        }

        // wait for the server message containing its info
        memset (buf, 0, INFO_PACKET_SIZE);
        ret = recvfrom (sock, buf, INFO_PACKET_SIZE, 0, NULL, NULL);
        if (ret < 0)
        {
            perror ("recvfrom");
            return -1;
        }

        memcpy (dest_mac, buf, ETH_ALEN);
        memcpy (dest_ip, buf + ETH_ALEN, sizeof (uint32_t));
    }
    else
    {
        // wait for the client message containing its info
        char buf[INFO_PACKET_SIZE];
        memset (buf, 0, INFO_PACKET_SIZE);
        ret = recvfrom (sock, buf, INFO_PACKET_SIZE, 0, NULL, NULL);
        if (ret < 0)
        {
            perror ("recvfrom");
            return -1;
        }

        memcpy (dest_mac, buf, ETH_ALEN);
        memcpy (dest_ip, buf + ETH_ALEN, sizeof (uint32_t));

        // send my info to the client
        struct sockaddr_in client_addr;
        client_addr.sin_family = AF_INET;
        client_addr.sin_port = htons (1234);
        client_addr.sin_addr.s_addr = *dest_ip;

        memcpy (buf, src_mac, ETH_ALEN);
        memcpy (buf + ETH_ALEN, src_ip, sizeof (uint32_t));
        ret = sendto (sock, buf, INFO_PACKET_SIZE, 0, (struct sockaddr *) &client_addr, sizeof (struct sockaddr_in));
        if (ret < 0)
        {
            perror ("sendto");
            return -1;
        }
    }

    close (sock);

    LOG (stdout, "Exchange of addresses completed!\n");
    return 0;
}

int build_base_packet (char *buf, const uint8_t *src_mac, const uint8_t *dest_mac,
                       const uint32_t src_ip, const uint32_t dest_ip)
{
    struct ethhdr *eth = (struct ethhdr *) buf;
    for (int i = 0; i < ETH_ALEN; ++i)
    {
        eth->h_source[i] = src_mac[i];
        eth->h_dest[i] = dest_mac[i];
    }

    eth->h_proto = __constant_htons (ETH_P_PINGPONG);

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
    ip->saddr = src_ip;
    ip->daddr = dest_ip;

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