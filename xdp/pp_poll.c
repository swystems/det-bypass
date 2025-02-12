#include "../common/common.h"
#include "../common/net.h"
#include "../common/persistence.h"
#include "src/args.h"
#include "src/xdp-loading.h"

#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>

#define DUMP_MAP 0

// Information about the XDP program
static const char *filename = "pingpong.o";
static const char *prog_name = "xdp_main";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong";
static const char *mapname = "last_payload";

static const char *outfile = "pingpong.dat";

volatile bool global_exit;

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
inline uint32_t poll_next_payload (volatile void *volatile map_ptr, struct pingpong_payload *dest_payload, uint32_t next_index)
{
    volatile struct pingpong_payload *volatile map = map_ptr;
    map += next_index;

    BUSY_WAIT (!valid_pingpong_payload (map) && !global_exit);
    memcpy (dest_payload, (void *) map, sizeof (struct pingpong_payload));

    memset ((void *) map, 0, sizeof (struct pingpong_payload));

    return (next_index + 1) % PACKETS_MAP_SIZE;
}

#if DUMP_MAP
struct dump_args {
    void *map_ptr;
    uint32_t *us_poll_idx;
    bool running;
};
/**
 * Dump the content of the eBPF map to the standard output.
 * This function should be run in a separate thread.
 */
void *dump_map (void *aux)
{
    struct dump_args *args = aux;
    while (args->running)
    {
        struct pingpong_payload *map = args->map_ptr;
        for (uint32_t i = 0; i < PACKETS_MAP_SIZE; ++i)
        {
            if (map->id == 0)
            {
                printf ("  EMPTY  ");
            }
            else
            {
                if (valid_pingpong_payload (map))
                    printf ("(%07llu)", map->id);// to be read
                else
                    printf ("[%07llu]", map->id);
            }
            printf (" ");
            map++;
        }
        printf ("\n");
        uint32_t us_idx = *args->us_poll_idx;
        for (uint32_t i = 0; i < PACKETS_MAP_SIZE; ++i)
        {
            if (i == us_idx)
                printf ("^^^^^^^^^ ");
            else
                printf ("          ");
        }
        printf ("\n");
        pp_sleep (500);
    }

    return NULL;
}
#endif

int send_packet (char *buf, const uint64_t packet_id, struct sockaddr_ll *sock_addr, void *aux)
{
    int sock = *(int *) aux;
    struct iphdr *ip = (struct iphdr *) (buf + sizeof (struct ethhdr));
    ip->id = htons (packet_id);
    struct pingpong_payload *payload = packet_payload (buf);
    *payload = new_pingpong_payload (packet_id);
    payload->ts[0] = get_time_ns ();

    return send_pingpong_packet (sock, buf, sock_addr);
}

#if !SERVER

static persistence_agent_t *persistence;

static void sigint_handler (int sig __unused)
{
    global_exit = true;
}

void start_client (uint64_t iters, uint64_t interval, struct sockaddr_ll *server_addr, char *base_packet, void *map_ptr)
{
    int sock = setup_socket ();
    if (sock < 0)
    {
        fprintf (stderr, "ERR: setup_socket failed\n");
        return;
    }

    signal (SIGINT, sigint_handler);

    LOG (stdout, "Starting sender thread... ");
    start_sending_packets (iters, interval, base_packet, server_addr, send_packet, &sock);
    LOG (stdout, "OK\n");

    // If the packet is not malloc'd, for some reason (??) nothing works.
    // I'm not sure, looks like the stack variable does not have the same size in memory as the one malloc'd. ???????
    struct pingpong_payload *buf_payload = malloc (sizeof (struct pingpong_payload));

    uint64_t current_id = 0;
    uint32_t next_map_idx = 0;

#if DUMP_MAP
    pthread_t map_dump_thread;
    struct dump_args *dump_map_args = malloc (sizeof (struct dump_args));
    dump_map_args->map_ptr = map_ptr;
    dump_map_args->us_poll_idx = &next_map_idx;
    dump_map_args->running = true;
    pthread_create (&map_dump_thread, NULL, dump_map, dump_map_args);
#endif

    while (current_id < iters && !global_exit)
    {
        next_map_idx = poll_next_payload (map_ptr, buf_payload, next_map_idx);
        if (UNLIKELY (global_exit))
            break;

        // LOG (stdout, "Packet: %llu %llu %llu %llu %llu %u\n", buf_payload->id, buf_payload->ts[0], buf_payload->ts[1], buf_payload->ts[2], buf_payload->ts[3], buf_payload->magic);

        if (buf_payload->phase != 2)
        {
            fprintf (stderr, "ERR: expected phase 2, got %d\n", buf_payload->phase);
            fprintf (stderr, "Packet: %llu %llu %llu %llu %llu\n", buf_payload->id, buf_payload->ts[0], buf_payload->ts[1], buf_payload->ts[2], buf_payload->ts[3]);
            continue;
        }

#if DEBUG
        if (buf_payload->id - current_id != 1)
            LOG (stderr, "WARN: missed %ld packets between %lu and %llu\n", (int64_t) buf_payload->id - (int64_t) current_id - 1, current_id, buf_payload->id);
#endif

        buf_payload->ts[3] = get_time_ns ();

        persistence->write (persistence, buf_payload);

        current_id = max (current_id, buf_payload->id);
    }

    LOG (stdout, "Global exit: %d\n", global_exit);

    pthread_cancel (get_sender_thread ());
    pthread_join (get_sender_thread (), NULL);

    close (sock);

#if DUMP_MAP
    dump_map_args->running = false;
    pthread_join (map_dump_thread, NULL);
    free (dump_map_args);
#endif

    munmap (map_ptr, sizeof (struct pingpong_payload) * PACKETS_MAP_SIZE);
}
#endif

#if SERVER
void start_server (const uint64_t iters, char *buf, struct sockaddr_ll *client_addr, void *map_ptr)
{
    int sock = setup_socket ();

    struct pingpong_payload *buf_payload = packet_payload (buf);

    uint64_t current_id = 0;
    uint32_t next_map_idx = 0;

    while (current_id < iters && !global_exit)
    {
        next_map_idx = poll_next_payload (map_ptr, buf_payload, next_map_idx);

        //LOG (stdout, "Packet: %llu %llu %llu %llu %llu %u\n", buf_payload->id, buf_payload->ts[0], buf_payload->ts[1], buf_payload->ts[2], buf_payload->ts[3], buf_payload->magic);
        if (UNLIKELY (buf_payload->phase != 0))
        {
            fprintf (stderr, "ERR: expected phase 0, got %d\n", buf_payload->phase);
            return;
        }

        buf_payload->ts[1] = get_time_ns ();

#if DEBUG
        if (buf_payload->id - current_id != 1)
            LOG (stderr, "WARN: missed %ld packets between %lu and %llu\n", (int64_t) buf_payload->id - (int64_t) current_id - 1, current_id, buf_payload->id);
#endif

        current_id = max (current_id, buf_payload->id);

        buf_payload->phase = 2;

        buf_payload->ts[2] = get_time_ns ();

        int ret = send_pingpong_packet (sock, buf, client_addr);

        if (UNLIKELY (ret < 0))
        {
            perror ("sendto");
            return;
        }

        if (UNLIKELY (current_id >= iters))
            break;
    }
}
#endif

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
 * @param interval only for the client, the interval between packets
 */
void start_pingpong (int ifindex, const char *server_ip, const uint64_t iters, const uint32_t interval)
{
    uint8_t src_mac[ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t dest_mac[ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint32_t src_ip = 0;
    uint32_t dest_ip = 0;
    LOG (stdout, "Exchanging addresses... ");
    int ret = exchange_eth_ip_addresses (ifindex, server_ip, SERVER, src_mac, dest_mac, &src_ip, &dest_ip);
    if (UNLIKELY (ret < 0))
    {
        fprintf (stderr, "ERR: exchange_eth_ip_addresses failed\n");
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

    char buf[PACKET_SIZE];
    memset (buf, 0, PACKET_SIZE);

    ret = build_base_packet (buf, src_mac, dest_mac, src_ip, dest_ip);
    if (ret < 0)
        return;

    struct sockaddr_ll sock_addr = build_sockaddr (ifindex, dest_mac);

#if SERVER
    start_server (iters, buf, &sock_addr, map_ptr);
#else
    start_client (iters, interval, &sock_addr, buf, map_ptr);
#endif
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
    char *ifname = argv[1];
    bool remove;
    uint64_t iters = 0;
    uint64_t interval = 0;
    char *server_ip = NULL;

#if SERVER
    if (!xdp_parse_args (argc, argv, &ifname, &remove, &iters))
    {
        xdp_print_usage (argv[0]);
        return EXIT_FAILURE;
    }
#else
    uint32_t persistence_flags = PERSISTENCE_M_ALL_TIMESTAMPS;

    if (!xdp_parse_args (argc, argv, &ifname, &remove, &iters, &interval, &server_ip, &persistence_flags))
    {
        xdp_print_usage (argv[0]);
        return EXIT_FAILURE;
    }

    persistence = persistence_init (outfile, persistence_flags, &interval);
    if (!persistence)
    {
        fprintf (stderr, "ERR: persistence_init failed\n");
        return EXIT_FAILURE;
    }
#endif

    int ifindex = if_nametoindex (ifname);
    if (!ifindex)
    {
        perror ("if_nametoindex");
        return EXIT_FAILURE;
    }

    if (remove)
    {
        int ret = detach_pingpong_xdp (ifindex);
        if (ret)
        {
            fprintf (stderr, "ERR: detaching program failed\n");
            return EXIT_FAILURE;
        }
        printf ("XDP program detached\n");
        return EXIT_SUCCESS;
    }

    detach_pingpong_xdp (ifindex);// always try to detach first

    // attach the pingpong XDP program
    int ret = attach_pingpong_xdp (ifindex);
    if (ret)
    {
        fprintf (stderr, "ERR: attaching program failed\n");
        return EXIT_FAILURE;
    }

    start_pingpong (ifindex, server_ip, iters, interval);

#if !SERVER
    if (persistence)
        persistence->close (persistence);
#endif

    return EXIT_SUCCESS;
}