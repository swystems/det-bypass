#include "../common/net.h"
#include "src/args.h"
#include "src/xdp-loading.h"
#include <stdint.h>
#include <stdio.h>

#include <stdbool.h>
#include <stdlib.h>

persistence_agent_t *persistence = NULL;

// Information about the XDP program
static const char *filename = "pingpong_pure.o";
static const char *prog_name = "xdp_main";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong_pure";

__attribute_maybe_unused__ static const char *outfile = "pingpong_pure.dat";

#if !SERVER

int send_packet (char *buf, uint64_t id, struct sockaddr_ll *server_addr, void *aux)
{
    int sock = *(int *) aux;
    struct pingpong_payload *payload = (struct pingpong_payload *) buf;
    *payload = new_pingpong_payload (id);
    payload->ts[0] = get_time_ns ();
    if (sendto (sock, buf, PACKET_SIZE, 0, (struct sockaddr *) server_addr, sizeof (*server_addr)) < 0)
    {
        perror ("sendto");
        return -1;
    }
    return 0;
}

int send_packets (int send_sock, const struct sockaddr_in *server_addr, uint64_t iters, uint64_t interval)
{
    char *packet = malloc (PACKET_SIZE);
    if (!packet)
    {
        perror ("malloc");
        return -1;
    }
    memset (packet, 0, PACKET_SIZE);

    start_sending_packets (iters, interval, packet, (struct sockaddr_ll *) server_addr, send_packet, &send_sock);

    return send_sock;
}

void receive_packets (int recv_sock, uint64_t iters)
{
    char packet[PACKET_SIZE];
    memset (packet, 0, PACKET_SIZE);

    uint64_t curr_iter = 0;
    while (curr_iter < iters)
    {
        if (recvfrom (recv_sock, packet, PACKET_SIZE, 0, NULL, NULL) < 0)
        {
            perror ("recvfrom");
            return;
        }

        struct pingpong_payload *payload = (struct pingpong_payload *) packet;
        persistence->write (persistence, payload);
        curr_iter = max (curr_iter, payload->id);
    }
}

void start_client (const char *server_ip, uint64_t iters, uint64_t interval)
{
    int send_sock = socket (AF_INET, SOCK_DGRAM, 0);
    if (send_sock < 0)
    {
        perror ("socket");
        return;
    }

    struct sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons (XDP_UDP_PORT);
    client_addr.sin_addr.s_addr = htonl (INADDR_ANY);

    if (bind (send_sock, (struct sockaddr *) &client_addr, sizeof (client_addr)) < 0)
    {
        perror ("bind");
        return;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons (XDP_UDP_PORT);
    server_addr.sin_addr.s_addr = inet_addr (server_ip);

    send_packets (send_sock, &server_addr, iters, interval);
    receive_packets (send_sock, iters);

    pthread_cancel (get_sender_thread ());
    pthread_join (get_sender_thread (), NULL);
    close (send_sock);

    persistence->close (persistence);
}
#endif

int main (int argc, char **argv)
{
    char *ifname = NULL;
    bool remove;
    uint64_t iters = 0;

#if SERVER
    if (!xdp_parse_args (argc, argv, &ifname, &remove, &iters))
    {
        xdp_print_usage (argv[0]);
        return EXIT_FAILURE;
    }
#else
    uint64_t interval = 0;
    char *server_ip = NULL;
    uint32_t persistence_flags = PERSISTENCE_M_ALL_TIMESTAMPS;

    if (!xdp_parse_args (argc, argv, &ifname, &remove, &iters, &interval, &server_ip, &persistence_flags))
    {
        xdp_print_usage (argv[0]);
        return EXIT_FAILURE;
    }

    //persistence_flags |= PERSISTENCE_F_STDOUT;
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
        fprintf (stderr, "ERR: if_nametoindex failed\n");
        return EXIT_FAILURE;
    }

    struct bpf_object *obj = read_xdp_file (filename);
    if (!obj)
    {
        fprintf (stderr, "ERR: loading file: %s\n", filename);
        return EXIT_FAILURE;
    }

    int ret = detach_xdp (obj, prog_name, ifindex, pinpath);
    if (remove)
    {
        return ret;
    }

    obj = read_xdp_file (filename);
//#if SERVER
    ret = attach_xdp (obj, prog_name, ifindex, pinpath);
    if (ret)
    {
        fprintf (stderr, "ERR: attach_xdp failed\n");
        return EXIT_FAILURE;
    }
//#endif

#if !SERVER
    start_client (server_ip, iters, interval);
#endif

    return EXIT_SUCCESS;
}