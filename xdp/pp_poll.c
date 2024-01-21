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

static uint64_t interval = 25000;
static persistence_agent_t *persistence;

// global variable to store the loaded xdp object
static struct bpf_object *loaded_xdp_obj;

/**
 * Poll the map until a new payload is found.
 * This function busy-waits until a new payload is found.
 * When a new payload is found, it is copied to the given destination payload and the map entry is cleared.
 *
 * @param map_ptr the pointer to the map
 * @param dest_payload the pointer to the payload to be filled
 * @param last_index the index of the last payload retrieved
 * @param busy_poll_next if true, the function busy-waits the next index, otherwise round-robin is used
 * @return the index of the retrieved payload
 */
inline int poll_next_payload (void *map_ptr, struct pingpong_payload *dest_payload, uint32_t last_index, bool busy_poll_next)
{
    uint32_t curr_idx = (last_index + 1) % PACKETS_MAP_SIZE;
    struct pingpong_payload *map = map_ptr;

    if (busy_poll_next)
    {
        BUSY_WAIT (!valid_pingpong_payload (map + curr_idx));
        *dest_payload = *(map + curr_idx);
        memset (map + curr_idx, 0, sizeof (struct pingpong_payload));
        return curr_idx;
    }

    while (1)
    {
        if (valid_pingpong_payload (map + curr_idx))
        {
            *dest_payload = *(map + curr_idx);
            memset (map + curr_idx, 0, sizeof (struct pingpong_payload));
            return curr_idx;
        }
        curr_idx = (curr_idx + 1) % PACKETS_MAP_SIZE;
    }
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
    bool is_server = server_ip == NULL;// if no server_ip is provided, I must be the server

    if (!is_server)
    {
        LOG (stdout, "Initializing persistence module... ");
        persistence = persistence_init (outfile, 0);
        if (!persistence)
        {
            fprintf (stderr, "ERR: persistence_init failed\n");
            return;
        }
        LOG (stdout, "OK\n");
    }

    uint8_t src_mac[ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t dest_mac[ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint32_t src_ip = 0;
    uint32_t dest_ip = 0;
    LOG (stdout, "Exchanging addresses... ");
    int ret = exchange_addresses (ifindex, server_ip, is_server, src_mac, dest_mac, &src_ip, &dest_ip);
    if (ret < 0)
    {
        fprintf (stderr, "ERR: exchange_addresses failed\n");
        return;
    }
    LOG (stdout, "OK\n");

    LOG (stdout, "Memory mapping BPF map... ");
    void *map_ptr = mmap_bpf_map (loaded_xdp_obj, mapname, sizeof (struct pingpong_payload) * PACKETS_MAP_SIZE);
    if (!map_ptr)
    {
        fprintf (stderr, "ERR: mmap_bpf_map failed\n");
        return;
    }
    LOG (stdout, "OK\n");

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
    {
        LOG (stdout, "Starting sender thread... ");
        start_sending_packets (sock, iters, interval, buf, &sock_addr);
        LOG (stdout, "OK\n");
    }

    // if client, send packet (phase 0), receive it back (phase 1), send it back (phase 2) and wait to receive it back again (phase 3)
    // then start again.
    // if server, wait to receive packet (phase 1), send it back (phase 2), repeat.
    printf ("\nStarting pingpong experiment... \n\n");
    fflush (stdout);
    uint32_t current_id = 0;
    uint32_t last_map_idx = -1;// -1 to make the first poll_next_payload start by looking in position 0

    struct pingpong_payload *buf_payload = packet_payload (buf);
    if (is_server)
    {
        while (current_id < iters)
        {
            last_map_idx = poll_next_payload (map_ptr, buf_payload, last_map_idx, current_id >= iters - PACKETS_MAP_SIZE);

            if (UNLIKELY (buf_payload->phase != 0))
            {
                fprintf (stderr, "ERR: expected phase 0, got %d\n", buf_payload->phase);
                return;
            }

            buf_payload->ts[1] = get_time_ns ();

#if DEBUG
            if (buf_payload->id - current_id != 1)
                LOG (stderr, "WARN: missed %d packets between %d and %d\n", buf_payload->id - current_id - 1, current_id, buf_payload->id);
#endif

            current_id = max (current_id, buf_payload->id);

            buf_payload->phase = 2;

            buf_payload->ts[2] = get_time_ns ();

            int ret = send_pingpong_packet (sock, buf, &sock_addr);

            if (UNLIKELY (ret < 0))
            {
                perror ("sendto");
                return;
            }

            if (UNLIKELY (current_id >= iters))
                break;
        }
    }
    else
    {
        while (current_id < iters)
        {
            last_map_idx = poll_next_payload (map_ptr, buf_payload, last_map_idx, current_id >= iters - PACKETS_MAP_SIZE);

            if (buf_payload->phase != 2)
            {
                fprintf (stderr, "ERR: expected phase 2, got %d\n", buf_payload->phase);
                return;
            }

#if DEBUG
            if (UNLIKELY (buf_payload->id - current_id != 1))
                LOG (stderr, "WARN: missed %d packets between %d and %d\n", buf_payload->id - current_id - 1, current_id, buf_payload->id);
#endif

            buf_payload->ts[3] = get_time_ns ();

            persistence->write (persistence, buf_payload);

            current_id = max (current_id, buf_payload->id);
        }
    }

    printf ("Pingpong experiment finished\n");

    close (sock);
    munmap (map_ptr, sizeof (struct pingpong_payload));
    if (persistence)
        persistence->close (persistence);
}

int attach_pingpong_xdp (int ifindex)
{
    LOG (stdout, "Attaching XDP program... ");
    struct bpf_object *obj = read_xdp_file (filename);
    if (!obj)
    {
        return -1;
    }

    loaded_xdp_obj = obj;
    int ret = attach_xdp (obj, prog_name, ifindex, pinpath);
    if (ret)
    {
        fprintf (stderr, "ERR: attaching program failed\n");
        return -1;
    }
    LOG (stdout, "OK\n");
    return ret;
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