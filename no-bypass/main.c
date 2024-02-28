#include "../common/common.h"
#include "../common/net.h"
#include "../common/persistence.h"
#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define PINGPONG_UDP_PORT 12345

persistence_agent_t *persistence_agent;

void usage (FILE *stream, char *prog)
{
    fprintf (stream, "Usage: %s -i <iters> [-n <interval ns> -s <server_ip>]\n", prog);
    fprintf (stream, "       -p, --packets: number of iterations\n");
    fprintf (stream, "       -i, --interval: interval between packets in ns\n");
    fprintf (stream, "       -s, --server: server IP address\n");
    fprintf (stream, "The client requires iters, interval and server IP; the server only requires iters\n");
    fprintf (stream, "Server example: %s -p 100\n", prog);
    fprintf (stream, "Client example: %s -p 100 -i 100000 -s 10.10.1.2\n", prog);
}

int new_socket (void)
{
    int fd = socket (AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        PERROR ("socket");
        return -1;
    }

    return fd;
}

struct sockaddr_in new_sockaddr (const char *ip, uint16_t port)
{
    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons (port);
    if (inet_pton (AF_INET, ip, &addr.sin_addr) <= 0)
    {
        PERROR ("inet_pton");
        exit (-1);
    }

    return addr;
}

void start_server (uint32_t iters)
{
    LOG (stdout, "Starting server\n");
    int socket = new_socket ();
    // wait for the client to connect
    struct sockaddr_in server_addr;
    memset (&server_addr, 0, sizeof (server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons (PINGPONG_UDP_PORT);
    server_addr.sin_addr.s_addr = htonl (INADDR_ANY);

    int ret = bind (socket, (struct sockaddr *) &server_addr, sizeof (server_addr));
    if (ret < 0)
    {
        PERROR ("bind");
        return;
    }

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof (client_addr);
    memset (&client_addr, 0, sizeof (client_addr));

    uint8_t recv_buf[PACKET_SIZE];
    uint32_t last_idx = 0;
    while (last_idx < iters)
    {
        int ret = recvfrom (socket, recv_buf, PACKET_SIZE, 0, (struct sockaddr *) &client_addr, &client_addr_len);
        uint64_t ts = get_time_ns ();
        if (ret < 0)
        {
            PERROR ("recvfrom");
            return;
        }

        struct pingpong_payload *payload = (struct pingpong_payload *) recv_buf;
        payload->ts[1] = ts;

        last_idx = max (last_idx, payload->id);

        payload->ts[2] = get_time_ns ();
        // send the packet back to the client
        ret = sendto (socket, recv_buf, PACKET_SIZE, 0, (struct sockaddr *) &client_addr, sizeof (client_addr));
        if (ret < 0)
        {
            PERROR ("sendto");
            return;
        }
    }
}

int send_single_packet (char *buf, const int packet_idx, struct sockaddr_ll *addr, void *aux)
{
    int socket = *(int *) aux;
    struct pingpong_payload *payload = (struct pingpong_payload *) buf;
    *payload = new_pingpong_payload (packet_idx);
    payload->ts[0] = get_time_ns ();

    return sendto (socket, buf, PACKET_SIZE, 0, (struct sockaddr *) addr, sizeof (*addr));
}

void start_client (uint32_t iters, uint64_t interval, const char *server_ip)
{
    LOG (stdout, "Starting client\n");
    int socket = new_socket ();
    struct sockaddr_in server_addr = new_sockaddr (server_ip, PINGPONG_UDP_PORT);
    uint8_t send_buf[PACKET_SIZE];
    memset (send_buf, 0, PACKET_SIZE);
    // passing a ref to the local socket should work fine since the current function will not return until the pingpong is finished.
    start_sending_packets (iters, interval, (char *) send_buf, (struct sockaddr_ll *) &server_addr, send_single_packet, &socket);

    uint8_t recv_buf[PACKET_SIZE];
    uint32_t last_idx = 0;
    while (last_idx < iters)
    {
        int ret = recvfrom (socket, recv_buf, PACKET_SIZE, 0, NULL, NULL);
        if (ret < 0)
        {
            PERROR ("recv");
            return;
        }

        struct pingpong_payload *payload = (struct pingpong_payload *) recv_buf;
        payload->ts[3] = get_time_ns ();
        persistence_agent->write (persistence_agent, payload);

        last_idx = max (last_idx, payload->id);
    }

    close (socket);
    persistence_agent->close (persistence_agent);
}

static struct option long_options[] = {
    {"packets", required_argument, 0, 'p'},
    {"interval", required_argument, 0, 'i'},
    {"server", required_argument, 0, 's'},
    {"measurement", required_argument, 0, 'm'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};

int main (int argc, char **argv)
{
    uint32_t iters = 0;
    uint64_t interval = 0;
    char *server_ip = NULL;
    int persistence_flag = PERSISTENCE_M_ALL_TIMESTAMPS;

    int opt;
    while ((opt = getopt_long (argc, argv, "p:i:s:m:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'p':
            iters = atoi (optarg);
            break;
        case 'i':
            interval = atoi (optarg);
            break;
        case 's':
            server_ip = optarg;
            break;
        case 'h':
            usage (stdout, argv[0]);
            return EXIT_SUCCESS;
        case 'm':
            persistence_flag = pers_measurement_to_flag (atoi (optarg));
            if (persistence_flag < 0)
            {
                fprintf (stderr, "Invalid measurement flag. See %s --help\n", argv[0]);
                return EXIT_FAILURE;
            }
            break;
        default:
            usage (stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    const bool is_server = server_ip == NULL;

    if (iters == 0 || (!is_server && interval == 0))
    {
        usage (stderr, argv[0]);
        return EXIT_FAILURE;
    }

    if (is_server)
        start_server (iters);
    else
    {
        persistence_agent = persistence_init ("no-bypass.dat", persistence_flag);
        if (!persistence_agent)
        {
            LOG (stderr, "Failed to initialize persistence agent\n");
            return EXIT_FAILURE;
        }

        start_client (iters, interval, server_ip);
    }

    return EXIT_SUCCESS;
}