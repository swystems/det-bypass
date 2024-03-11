#include "../common/common.h"
#include "../common/net.h"
#include "../common/persistence.h"
#include "src/args.h"
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

int main (int argc, char **argv)
{
#if SERVER
    uint32_t iters = 0;
    if (!nobypass_parse_args (argc, argv, &iters))
    {
        nobypass_print_usage (argv[0]);
        return EXIT_FAILURE;
    }

    LOG (stdout, "Starting server with iters=%u\n", iters);

    start_server (iters);
#else
    uint32_t persistence_flag = PERSISTENCE_M_ALL_TIMESTAMPS;

    uint32_t iters = 0;
    uint64_t interval = 0;
    char *server_ip = NULL;
    if (!nobypass_parse_args (argc, argv, &iters, &interval, &server_ip, &persistence_flag))
    {
        nobypass_print_usage (argv[0]);
        return EXIT_FAILURE;
    }

    persistence_agent = persistence_init ("no-bypass.dat", persistence_flag);
    if (!persistence_agent)
    {
        LOG (stderr, "Failed to initialize persistence agent\n");
        return EXIT_FAILURE;
    }

    LOG (stdout, "Starting client with iters=%u, interval=%lu, server_ip=%s\n", iters, interval, server_ip);

    start_client (iters, interval, server_ip);
#endif

    return EXIT_SUCCESS;
}