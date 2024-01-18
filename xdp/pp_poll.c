#include "common.h"
#include "src/net.h"
#include "src/persistence.h"
#include "src/xdp-loading.h"

#include <stdbool.h>
#include <stdlib.h>

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

static const char *outfile = "pingpong.dat";

// global variable to store the loaded xdp object
static struct bpf_object *loaded_xdp_obj;

/**
 * Busy-poll until a new payload is available in the BPF map.
 * If the values inside the map change faster than this function is called/can poll,
 * the payload will be overwritten and lost.
 *
 * @param map_ptr the pointer to the BPF map
 * @return the next payload
 */
inline struct pingpong_payload poll_next_payload (void *map_ptr)
{
    struct pingpong_payload *payload = (struct pingpong_payload *) map_ptr;
    uint32_t last_id = payload->id;
    uint32_t last_phase = payload->phase;
    BUSY_WAIT (payload->id == last_id && payload->phase == last_phase);
    return *payload;
}

/**
 * Start the pingpong experiment between the current node and the remote node.
 *
 * If server_ip is NULL, the current node is the server. Otherwise, it is the client.
 *
 * First, client and server exchange each other's mac and ip addresses using UDP packets.
 * Then, the client starts a thread that sends `iters` packets to the server.
 * In the meanwhile, both the client and server will be running this function that keeps polling
 * the BPF map to retrieve the last received packet.
 *
 * The pingpong can be visualized as follows:
 *     Client                 Server
 *  ╔══════════╗           ╔══════════╗
 *  ║          ║           ║          ║
 *  ║  Phase 0 ║  ──────>  ║ Phase 1  ║
 *  ║          ║           ║          ║
 *  ║  Phase 3 ║  <──────  ║ Phase 2  ║
 *  ║          ║           ║          ║
 *  ╚══════════╝           ╚══════════╝
 *
 * Phase 0 is the packet being created by the client and sent to the server.
 * Timestamp 0 contains the client PING TX timestamp of the packet.
 *
 * Phase 1 is the packet being received by the server.
 * Timestamp 1 contains the server PING RX timestamp of the packet.
 *
 * Phase 2 is the packet being sent back by the server.
 * Timestamp 2 contains the server PONG TX timestamp of the packet.
 *
 * Phase 3 is the packet being received back by the client.
 * Timestamp 3 contains the client PONG RX timestamp of the packet.
 *
 * @param ifindex the interface index
 * @param server_ip the server IP address
 * @param iters the number of packets to send
 */
void start_pingpong (int ifindex, const char *server_ip, const uint32_t iters)
{
    printf ("Starting pingpong\n");

    persistence_init (outfile);

    bool is_server = server_ip == NULL;// if no server_ip is provided, I must be the server

    uint8_t src_mac[ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t dest_mac[ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint32_t src_ip = 0;
    uint32_t dest_ip = 0;
    int ret = exchange_addresses (ifindex, server_ip, is_server, src_mac, dest_mac, &src_ip, &dest_ip);
    if (ret < 0)
    {
        fprintf (stderr, "ERR: exchange_addresses failed\n");
        return;
    }

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

    ret = build_base_packet (buf, src_mac, dest_mac, src_ip, dest_ip);
    if (ret < 0)
        return;

    struct sockaddr_ll sock_addr = build_sockaddr (ifindex, dest_mac);

    if (!is_server)
        start_sending_packets (sock, iters, 100000, buf, &sock_addr);

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

            current_id = max (current_id, payload.id);

            payload.ts[2] = get_time_ns ();

            payload.phase = 2;

            set_packet_payload (buf, &payload);
            int ret = send_pingpong_packet (sock, buf, &sock_addr);
            if (ret < 0)
            {
                perror ("sendto");
                return;
            }

            if (current_id >= iters)
                break;
        }
        else
        {
            struct pingpong_payload payload = poll_next_payload (map_ptr);
            if (payload.phase != 2)
            {
                fprintf (stderr, "ERR: expected phase 2, got %d\n", payload.phase);
                return;
            }

            payload.ts[3] = get_time_ns ();

            persistence_write (&payload);

            current_id = max (current_id, payload.id);
        }
    }

    LOG (stdout, "Pingpong experiment finished\n");
    close (sock);
    munmap (map_ptr, sizeof (struct pingpong_payload));
    persistence_close ();

    return;
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

        // attach the pingpong XDP program
        int ret = attach_pingpong_xdp (ifindex);
        if (ret)
        {
            fprintf (stderr, "ERR: attaching program failed\n");
            return EXIT_FAILURE;
        }
        printf ("XDP program attached\n");

        const int iters = atoi (argv[3]);
        char *ip = argc > 4 ? argv[4] : NULL;// not required, only for the client

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