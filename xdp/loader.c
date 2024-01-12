#include "common.h"
#include "xdp-loading.h"
#include <arpa/inet.h>
#include <bpf/libbpf.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <net/if.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

static void update_rlimit (void)
{
    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
    int ret = setrlimit (RLIMIT_MEMLOCK, &r);
    if (ret)
    {
        perror ("setrlimit");
        fprintf (stderr, "ERR: setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY) failed\n");
        fprintf (stderr, "Try with sudo\n");
        exit (EXIT_FAILURE);
    }
}

void usage (char *prog)
{
    // <prog> <ifname> <action>
    fprintf (stderr, "Usage: %s <ifname> <action> [extra arguments]\n", prog);
    fprintf (stderr, "Actions:\n");
    fprintf (stderr, "\t- start: start the pingpong experiment\n");
    fprintf (stderr, "\t         only on the client machine, it requires two extra arguments: <number of packets> <server IP>\n");
    fprintf (stderr, "\t- remove: remove XDP program\n");
}

static const char *filename = "pingpong.o";
static const char *prog_name = "xdp_main";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong";
static const char *mapname = "last_timestamp";

static struct pingpong_payload *payloads;
static int iters = 0;

static struct bpf_object *loaded_xdp_obj;

volatile bool is_polling = false;

/**
 * Continuously poll the XDP map for the latest pingpong_payload value and print it.
 */
void *poll_thread (void *aux __attribute__ ((unused)))
{
    int map_fd = bpf_object__find_map_fd_by_name (loaded_xdp_obj, mapname);
    if (map_fd < 0)
    {
        fprintf (stderr, "ERR: finding map failed\n");
        return NULL;
    }

    void *map = mmap (NULL, sizeof (struct pingpong_payload), PROT_READ, MAP_SHARED, map_fd, 0);
    if (map == MAP_FAILED)
    {
        fprintf (stderr, "ERR: mmap failed\n");
        return NULL;
    }

    // notify the program that the polling thread is ready
    is_polling = true;

    uint32_t last_id = 0;
    struct pingpong_payload *payload = map;
    while (payload->id < iters)
    {
        printf ("ID %d: %llu %llu %llu %llu\n", payload->id, payload->ts[0], payload->ts[1], payload->ts[2],
                payload->ts[3]);
        last_id = payload->id;

        // wait until we get a new packet
        BUSY_WAIT (payload->id == last_id);
    }

    munmap (map, sizeof (struct pingpong_payload));

    printf ("Poll thread finished\n");
    return NULL;
}

pthread_t start_poll_thread (void)
{
    // Create and start a thread with poll_thread function
    pthread_t thread;
    pthread_create (&thread, NULL, poll_thread, NULL);

    BUSY_WAIT (!is_polling);

    return thread;
}

void send_packets (int ifindex, const char *server_ip)
{
    char src_mac[ETH_ALEN] = {0x9c, 0xdc, 0x71, 0x5d, 0x51, 0x31};
    char dest_mac[ETH_ALEN] = {0x9c, 0xdc, 0x71, 0x5d, 0xd5, 0xf1};

    //    const uint32_t src_ip = inet_addr ("10.10.1.2");
    const uint32_t dest_ip = inet_addr (server_ip);

    int sock = socket (AF_PACKET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0)
    {
        perror ("socket");
        return;
    }

    char buf[PACKET_SIZE];
    memset (buf, 0, PACKET_SIZE);

    // Ethernet header
    struct ethhdr *eth = (struct ethhdr *) buf;
    for (int i = 0; i < ETH_ALEN; ++i)
    {
        eth->h_source[i] = src_mac[i];
        eth->h_dest[i] = dest_mac[i];
    }
    eth->h_proto = __constant_htons (0x2002);

    struct iphdr *ip = (struct iphdr *) (eth + 1);
    ip->ihl = 5;
    ip->version = 4;
    ip->tos = 0;
    ip->tot_len = htons (PACKET_SIZE - sizeof (struct ethhdr));
    ip->id = htons (0);
    ip->frag_off = htons (0);
    ip->ttl = 64;
    ip->protocol = IPPROTO_RAW;
    ip->check = 0;
    //    ip->saddr = src_ip;
    ip->daddr = dest_ip;

    struct sockaddr_ll sock_addr;
    sock_addr.sll_ifindex = ifindex;
    sock_addr.sll_halen = ETH_ALEN;
    for (int i = 0; i < ETH_ALEN; ++i)
        sock_addr.sll_addr[i] = dest_mac[i];

    for (int i = 0; i < iters; ++i)
    {
        ip->id = htons (i);

        struct pingpong_payload *payload = (struct pingpong_payload *) (ip + 1);
        payload->id = i;
        payload->phase = 0;
        payload->ts[0] = get_time_ns ();

        int ret = sendto (sock, buf, PACKET_SIZE, 0, (struct sockaddr *) &sock_addr, sizeof (struct sockaddr_ll));
        if (ret < 0)
        {
            perror ("sendto");
            return;
        }

        long long start = get_time_ns ();
        BUSY_WAIT (get_time_ns () - start < 20000);
    }

    close (sock);
}

void start_pingpong (int ifindex, const char *server_ip)
{
    const pthread_t thread = start_poll_thread ();

    send_packets (ifindex, server_ip);

    pthread_join (thread, NULL);
}

int attach_pingpong_xdp (int ifindex)
{
    struct bpf_object *obj = read_xdp_file (filename);
    if (!obj)
    {
        return -1;
    }

    loaded_xdp_obj = obj;
    return attach_xdp (obj, prog_name, ifindex, pinpath);
}

int detach_pingpong_xdp (int ifindex)
{
    struct bpf_object *obj = read_xdp_file (filename);
    if (!obj)
    {
        return -1;
    }

    return detach_xdp (obj, prog_name, ifindex, pinpath);
}

int main (int argc, char **argv)
{
    update_rlimit ();

    if (argc < 3)
    {
        usage (argv[0]);
        return EXIT_FAILURE;
    }

    char *ifname = argv[1];
    char *action = argv[2];

    int ifindex = if_nametoindex (ifname);
    if (!ifindex)
    {
        perror ("if_nametoindex");
        return EXIT_FAILURE;
    }

    if (strcmp (action, "start") == 0)
    {
        detach_pingpong_xdp (ifindex);
        int ret = attach_pingpong_xdp (ifindex);
        printf ("XDP program attached\n");
        if (ret)
        {
            fprintf (stderr, "ERR: attaching program failed\n");
            return EXIT_FAILURE;
        }

        if (argc == 3)
        {
            // the client does not need to do anything else (for now!)
            return EXIT_SUCCESS;
        }
        else if (argc < 5)
        {
            usage (argv[0]);
            return EXIT_FAILURE;
        }

        iters = atoi (argv[3]);
        char *ip = argc > 4 ? argv[4] : NULL;

        payloads = calloc (iters, sizeof (struct pingpong_payload));
        if (!payloads)
        {
            perror ("malloc");
            return EXIT_FAILURE;
        }

        start_pingpong (ifindex, ip);
    }
    else if (strcmp (action, "remove") == 0)
    {
        int ret = detach_pingpong_xdp (ifindex);
        if (ret)
        {
            fprintf (stderr, "ERR: detaching program failed\n");
            return EXIT_FAILURE;
        }
        printf ("XDP program detached\n");
    }
    else
    {
        usage (argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}