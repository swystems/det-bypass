#include "common.h"
#include "src/net.h"
#include "src/xdp-loading.h"

#include <bpf/libbpf.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>

void usage (char *prog)
{
    // <prog> <ifname> <action>
    fprintf (stderr, "Usage: %s <ifname> <action> [extra arguments]\n", prog);
    fprintf (stderr, "Actions:\n");
    fprintf (stderr, "\t- start: start the pingpong experiment\n");
    fprintf (stderr, "\t         only on the client machine, it requires two extra arguments: <number of packets> <server IP>\n");
    fprintf (stderr, "\t- remove: remove XDP program\n");
}

// Information about the XDP program
static const char *filename = "pingpong.o";
static const char *prog_name = "xdp_main";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong";
static const char *mapname = "last_payload";

// global variable to store the loaded xdp object
static struct bpf_object *loaded_xdp_obj;

inline struct pingpong_payload poll_next_payload (void *map_ptr)
{
    struct pingpong_payload *payload = (struct pingpong_payload *) map_ptr;
    uint32_t last_id = payload->id;
    uint32_t last_phase = payload->phase;
    BUSY_WAIT (payload->id == last_id && payload->phase == last_phase);
    return *payload;
}

void start_pingpong (int ifindex, const char *server_ip, const uint32_t iters)
{
    LOG (stdout, "Starting pingpong experiment...\n");
    // "src" and "dest" are from the point of view of the client
    // if I am the server, they must be swapped
    const uint8_t client_mac[ETH_ALEN] = {0x0c, 0x42, 0xa1, 0xdd, 0x60, 0xb0};
    const uint8_t server_mac[ETH_ALEN] = {0x0c, 0x42, 0xa1, 0xe2, 0xa6, 0xa8};

    const char *client_ip = "10.10.1.1";

    bool is_server = server_ip == NULL;// if no server_ip is provided, I must be the server

    const uint8_t *src_mac = is_server ? server_mac : client_mac;
    const uint8_t *dest_mac = is_server ? client_mac : server_mac;
    const char *src_ip = is_server ? "10.10.1.2" : client_ip;
    const char *dest_ip = is_server ? client_ip : server_ip;

    void *map_ptr = mmap_bpf_map (loaded_xdp_obj, mapname, sizeof (struct pingpong_payload));
    if (!map_ptr)
    {
        fprintf (stderr, "ERR: mmap_bpf_map failed\n");
        return;
    }

    int sock = setup_socket ();
    if (sock < 0)
    {
        fprintf (stderr, "ERR: setup_socket failed\n");
        return;
    }

    char buf[PACKET_SIZE];
    memset (buf, 0, PACKET_SIZE);

    int ret = build_base_packet (buf, src_mac, dest_mac, src_ip, dest_ip);
    if (ret < 0)
        return;

    LOG (stdout, "Starting pingpong, source IP address: %s, destination IP address: %s\n", src_ip, dest_ip);

    // if client, send packet (phase 0), receive it back (phase 1), send it back (phase 2) and wait to receive it back again (phase 3)
    // then start again.
    // if server, wait to receive packet (phase 1), send it back (phase 2), repeat.
    uint32_t current_id = 1;
    while (current_id < iters)
    {
        if (is_server)
        {
            struct pingpong_payload payload = poll_next_payload (map_ptr);

            uint64_t receive_time = get_time_ns ();
            if (payload.phase != 0)
            {
                fprintf (stderr, "ERR: expected phase 0, got %d\n", payload.phase);
                return;
            }

            payload.ts[1] = receive_time;

            current_id = payload.id;

            payload.ts[2] = get_time_ns ();

            payload.phase = 2;

            set_packet_payload (buf, &payload);
            int ret = send_pingpong_packet (sock, buf, ifindex, dest_mac);
            if (ret < 0)
            {
                perror ("sendto");
                return;
            }
        }
        else
        {
            struct pingpong_payload payload = {0};
            payload.phase = 0;
            payload.id = current_id;
            payload.ts[0] = get_time_ns ();
            set_packet_payload (buf, &payload);

            int ret = send_pingpong_packet (sock, buf, ifindex, dest_mac);
            if (ret < 0)
            {
                perror ("sendto");
                return;
            }

            payload = poll_next_payload (map_ptr);
            if (payload.phase != 2)
            {
                fprintf (stderr, "ERR: expected phase 2, got %d\n", payload.phase);
                return;
            }
            if (payload.id != current_id)
            {
                fprintf (stderr, "ERR: expected id %d, got %d\n", current_id, payload.id);
                return;
            }

            payload.ts[3] = get_time_ns ();
            printf ("Packet %d: %llu %llu %llu %llu\n", payload.id, payload.ts[0], payload.ts[1], payload.ts[2], payload.ts[3]);

            ++current_id;
            usleep (20);
        }
    }
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
#if DEBUG
    printf ("DEBUG mode\n");
#endif
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
        detach_pingpong_xdp (ifindex);// always try to detach first

        int ret = attach_pingpong_xdp (ifindex);
        if (ret)
        {
            fprintf (stderr, "ERR: attaching program failed\n");
            return EXIT_FAILURE;
        }
        printf ("XDP program attached\n");

        const int iters = atoi (argv[3]);
        char *ip = argc > 4 ? argv[4] : NULL;

        start_pingpong (ifindex, ip, iters);
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