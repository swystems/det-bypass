#include "common.h"
#include <arpa/inet.h>
#include <bpf/libbpf.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/perf_event.h>
#include <net/if.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    fprintf (stderr, "action:\n");
    fprintf (stderr, "  start: load, attach, pin and start recording pingpong\n");
    fprintf (stderr, "         requires an extra argument: <num of packets>\n");
    fprintf (stderr, "  remove: remove pinned program\n");
}

static const char *filename = "pingpong.o";
static const char *prog_name = "xdp_main";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong";
static const char *mapname = "last_timestamp";
static struct bpf_object *obj;

static struct pingpong_payload *payloads;
static int iters = 0;

long long get_time_ns (void)
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

/**
 * Remove the XDP program from the given interface.
 *
 * @param ifindex The interface index to remove the program from. If an interface with this index does not exist, the
 * function will fail and the program will exit.
 * @return EXIT_SUCCESS if the program was successfully removed, EXIT_FAILURE otherwise.
 */
int remove_pingpong (int ifindex)
{
    obj = bpf_object__open_file (filename, NULL);
    if (!obj)
    {
        fprintf (stderr, "ERR: opening file failed\n");
        return EXIT_FAILURE;
    }

    int ret = bpf_object__load (obj);
    if (ret)
    {
        fprintf (stderr, "ERR: loading file failed\n");
        return EXIT_FAILURE;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name (obj, prog_name);
    if (!prog)
    {
        fprintf (stderr, "ERR: finding program failed\n");
        return EXIT_FAILURE;
    }

    ret = bpf_program__unpin (prog, pinpath);// ret can be ignored, if unpin fails, it's not a big deal

    ret = bpf_xdp_detach (ifindex, XDP_FLAGS_DRV_MODE, 0);
    if (ret)
    {
        fprintf (stderr, "ERR: detaching program failed\n");
        return EXIT_FAILURE;
    }

    bpf_program__unload (prog);

    printf ("Program detached from interface %d\n", ifindex);

    return EXIT_SUCCESS;
}

/**
 * Attach the XDP program to the given interface.
 *
 * The XDP program defined by `filename` and `prog_name` is loaded and attached to the interface in "driver" mode.
 * After being loaded, the program is also pinned to the BPF filesystem.
 *
 * @param ifindex The interface index to attach the program to. If an interface with this index does not exist, the
 * function will fail and the program will exit.
 */
int attach_xdp (int ifindex, bool retry)
{
    obj = bpf_object__open_file (filename, NULL);
    if (!obj)
    {
        fprintf (stderr, "ERR: opening file failed\n");
        perror ("bpf_object__open_file");
        return EXIT_FAILURE;
    }

    int ret = bpf_object__load (obj);
    if (ret)
    {
        fprintf (stderr, "ERR: loading file failed\n");
        perror ("bpf_object__load");
        return EXIT_FAILURE;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name (obj, prog_name);
    if (!prog)
    {
        fprintf (stderr, "ERR: finding program failed\n");
        return EXIT_FAILURE;
    }

    ret = bpf_program__pin (prog, pinpath);
    if (ret)
    {
        // try to remove the pingpong only if it's the first time doing so.
        if (!retry)
            ret = remove_pingpong (ifindex);
        // if we had already done this, stop to avoid infinite recursion
        // if there was an error removing the program, we can't do anything about it and exit
        if (retry || ret)
        {
            fprintf (stderr, "ERR: pinning program failed\n");
            return EXIT_FAILURE;
        }
        return attach_xdp (ifindex, true);
    }

    ret = bpf_xdp_attach (ifindex, bpf_program__fd (prog), XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_DRV_MODE, 0);
    if (ret)
    {
        fprintf (stderr, "ERR: attaching program failed\n");
        return EXIT_FAILURE;
    }
    printf ("Program attached to interface %d\n", ifindex);

    return EXIT_SUCCESS;
}

/**
 * Continuously poll the XDP map for the latest pingpong_payload value and print it.
 */
void *poll_thread (void *aux __attribute__ ((unused)))
{
    int map_fd = bpf_object__find_map_fd_by_name (obj, mapname);
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

    uint32_t last_id = 0;
    struct pingpong_payload *payload = map;
    while (payload->id < iters - 1)
    {
        printf ("ID %d: %llu %llu %llu %llu\n", payload->id, payload->ts[0], payload->ts[1], payload->ts[2],
                payload->ts[3]);
        last_id = payload->id;

        // busy_wait
        while (payload->id == last_id) {}
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
    return thread;
}

void send_packets (int ifindex, const char *server_ip)
{
    // send a raw packet to server_ip containing an Ethernet header with type 0x2002 and empty addresses, an IP header
    // with the given server_ip as destination and a pingpong_payload with phase 0 and id 0

    // create a raw socket
    int sock = socket (AF_PACKET, SOCK_RAW, htons (ETH_P_ALL));
    if (sock < 0)
    {
        perror ("socket");
        return;
    }

    // create a sockaddr_ll struct with the interface index
    struct sockaddr_ll addr = {
        .sll_family = AF_PACKET,
        .sll_ifindex = ifindex,
        .sll_halen = ETH_ALEN,
    };

    // create an Ethernet header with type 0x2002 and empty addresses
    struct ethhdr eth = {
        .h_proto = htons (0x2002),
    };

    // create an IP header with the given server_ip as destination
    struct iphdr ip = {
        .version = 4,
        .ihl = 5,
        .ttl = 64,
        .protocol = IPPROTO_UDP,
        .daddr = inet_addr (server_ip),
    };

    // create a pingpong_payload with phase 0 and id 0
    struct pingpong_payload payload = {
        .phase = 0,
        .id = 0,
    };

    // create a buffer containing the Ethernet header, the IP header and the pingpong_payload
    char buf[PACKET_SIZE];
    memcpy (buf, &eth, sizeof (eth));
    memcpy (buf + sizeof (eth), &ip, sizeof (ip));
    memcpy (buf + sizeof (eth) + sizeof (ip), &payload, sizeof (payload));

    // send the buffer to the server
    int ret = sendto (sock, buf, sizeof (buf), 0, (struct sockaddr *) &addr, sizeof (addr));
    if (ret < 0)
    {
        perror ("sendto");
        return;
    }

    // close the socket
    close (sock);
}

void start_pingpong (int ifindex, const char *server_ip)
{
    int ret = attach_xdp (ifindex, false);
    if (ret)
    {
        fprintf (stderr, "ERR: attaching program failed\n");
        return;
    }
    bool is_server = server_ip == NULL;
    if (is_server)
        return;// the server does not need to do anything packet-wise, it just needs to echo using XDP_TX

    const pthread_t thread = start_poll_thread ();

    send_packets (ifindex, server_ip);

    pthread_join (thread, NULL);
}

int remove_pingpong (int ifindex)
{
    obj = bpf_object__open_file (filename, NULL);
    if (!obj)
    {
        fprintf (stderr, "ERR: opening file failed\n");
        return EXIT_FAILURE;
    }

    int ret = bpf_object__load (obj);
    if (ret)
    {
        fprintf (stderr, "ERR: loading file failed\n");
        return EXIT_FAILURE;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name (obj, prog_name);
    if (!prog)
    {
        fprintf (stderr, "ERR: finding program failed\n");
        return EXIT_FAILURE;
    }

    ret = bpf_program__unpin (prog, pinpath);// ret can be ignored, if unpin fails, it's not a big deal

    ret = bpf_xdp_detach (ifindex, XDP_FLAGS_DRV_MODE, 0);
    if (ret)
    {
        fprintf (stderr, "ERR: detaching program failed\n");
        return EXIT_FAILURE;
    }

    bpf_program__unload (prog);

    printf ("Program detached from interface %d\n", ifindex);

    return EXIT_SUCCESS;
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
        iters = atoi (argv[3]);
        char *ip = argc > 4 ? argv[4] : NULL;

        payloads = calloc (iters, PACKET_SIZE);
        if (!payloads)
        {
            perror ("malloc");
            return EXIT_FAILURE;
        }

        start_pingpong (ifindex, ip);
    }
    else if (strcmp (action, "remove") == 0)
    {
        remove_pingpong (ifindex);
    }
    else
    {
        usage (argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}