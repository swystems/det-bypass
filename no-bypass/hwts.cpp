#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using namespace std;

#ifndef SERVER
#define SERVER 0
#endif

#if CLIENT
#define SERVER 0
#endif

using u64 = uint64_t;

static constexpr int PACKET_SIZE = 1024;

static constexpr int PORT = 12345;

vector<u64> send_ts;
u64 send_ts_cnt = 0;
vector<u64> recv_ts;
u64 recv_ts_cnt = 0;

sockaddr_in generate_sockaddr (string &remote)
{
    struct sockaddr_in sockaddr;
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_port = htons (PORT);
    inet_pton (AF_INET, remote.c_str (), &sockaddr.sin_addr);
    return sockaddr;
}

int set_hw_timestamps (int sock, string ifname, bool enabled)
{
    struct ifreq ifr;
    struct hwtstamp_config hwconfig;
    memset (&ifr, 0, sizeof (ifr));
    memset (&hwconfig, 0, sizeof (hwconfig));
    strncpy (ifr.ifr_name, ifname.c_str (), IFNAMSIZ - 1);
    ifr.ifr_data = (char *) &hwconfig;
    if (enabled)
    {
        hwconfig.tx_type = HWTSTAMP_TX_ON;
        hwconfig.rx_filter = HWTSTAMP_FILTER_ALL;
    }
    else
    {
        hwconfig.tx_type = HWTSTAMP_TX_OFF;
        hwconfig.rx_filter = HWTSTAMP_FILTER_NONE;
    }

    if (ioctl (sock, SIOCSHWTSTAMP, &ifr) < 0)
    {
        perror ("ioctl");
        return -1;
    }
    return 0;
}

void set_socket_timestamps (int socket_fd)
{
    int timestamp_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE
                          | SOF_TIMESTAMPING_OPT_ID;
    setsockopt (socket_fd, SOL_SOCKET, SO_TIMESTAMPING, &timestamp_flags, sizeof (timestamp_flags));
}

void extract_recv_timestamp (msghdr *msg)
{
    cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR (msg); cmsg; cmsg = CMSG_NXTHDR (msg, cmsg))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING)
        {
            auto *ts = (struct timespec *) CMSG_DATA (cmsg);
            u64 val = ts[2].tv_sec * 1000000000 + ts[2].tv_nsec;
            recv_ts[recv_ts_cnt++] = val;
        }
    }
}

void extract_send_timestamps (msghdr *msg)
{
    cmsghdr *cmsg;
    __u32 idx = -1;
    u64 _ts = -1;
    for (cmsg = CMSG_FIRSTHDR (msg); cmsg; cmsg = CMSG_NXTHDR (msg, cmsg))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING)
        {
            auto *ts = (struct timespec *) CMSG_DATA (cmsg);
            u64 val = ts[2].tv_sec * 1000000000 + ts[2].tv_nsec;
            if (idx != -1)
            {
                send_ts[idx] = val;
                idx = -1;
                ++send_ts_cnt;
            }
            else
                _ts = val;
        }
        else if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR)
        {
            auto *err = (struct sock_extended_err *) CMSG_DATA (cmsg);
            if (err->ee_origin != SO_EE_ORIGIN_TIMESTAMPING)
                continue;
            if (idx != -1)
            {
                throw runtime_error ("Received two packet ids but no timestamp");
            }
            idx = err->ee_data;
            if (_ts != -1)
            {
                send_ts[idx] = _ts;
                idx = -1;
                ++send_ts_cnt;
            }
        }
    }
}

void extract_send_timestamps (int sock, sockaddr_in *addr)
{
    char control[2048];
    char buffer[0];
    struct iovec iov {
        .iov_base = buffer, .iov_len = 0,
    };
    msghdr msg{
        .msg_name = addr,
        .msg_namelen = sizeof (sockaddr_in),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof (control),
    };

    ssize_t res = recvmsg (sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (res < 0 && errno != EAGAIN)
    {
        cout << errno << endl;
        perror ("recvmsg");
        return;
    }

    extract_send_timestamps (&msg);
}

void receive (int socket, sockaddr_in *addr)
{
    char buffer[PACKET_SIZE];
    char control[2048];
    struct iovec iov {
        .iov_base = buffer, .iov_len = PACKET_SIZE,
    };
    msghdr msg{
        .msg_name = addr,
        .msg_namelen = sizeof (sockaddr_in),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof (control),
    };

    ssize_t res = recvmsg (socket, &msg, 0);
    if (res < 0 && errno != EAGAIN)
    {
        perror ("recvmsg");
        return;
    }

    extract_recv_timestamp (&msg);
}

void send (int socket, sockaddr_in *addr)
{
    char buffer[PACKET_SIZE];
    struct iovec iov {
        .iov_base = buffer, .iov_len = PACKET_SIZE,
    };
    msghdr msg{.msg_name = addr, .msg_namelen = sizeof (sockaddr_in), .msg_iov = &iov, .msg_iovlen = 1, .msg_control = nullptr, .msg_controllen = 0};

    ssize_t res = sendmsg (socket, &msg, 0);
    if (res < 0 && errno != EAGAIN)
    {
        perror ("sendmsg");
        return;
    }
}

void server_loop (int socket, string &remote, u64 packets, u64 interval)
{
    sockaddr_in remote_sockaddr = generate_sockaddr (remote);

    for (int i = 0; i < packets; ++i)
    {
        //        cout << "Waiting for packet " << i << endl;
        receive (socket, &remote_sockaddr);
        //        cout << "Received packet " << i << endl;

        extract_send_timestamps (socket, &remote_sockaddr);

        //cout << "Sending packet " << i << endl;
        send (socket, &remote_sockaddr);
        //cout << "Sent packet " << i << endl;
    }
}

void client_loop (int socket, string &remote, u64 packets, u64 interval)
{
    sockaddr_in sockaddr = generate_sockaddr (remote);
    socklen_t sockaddr_len = sizeof (sockaddr);

    for (int i = 0; i < packets; ++i)
    {
        //        cout << "Sending packet " << i << endl;
        send (socket, &sockaddr);
        //        cout << "Sent packet " << i << endl;

        extract_send_timestamps (socket, &sockaddr);

        //        cout << "Waiting for packet " << i << endl;
        receive (socket, &sockaddr);
        //        cout << "Received packet " << i << endl;
    }
}

int start_program (string ifname, string remote, u64 packets, u64 interval)
{
    int socket_fd = socket (AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        perror ("socket");
        return EXIT_FAILURE;
    }

    // bind to device
    if (setsockopt (socket_fd, SOL_SOCKET, SO_BINDTODEVICE, ifname.c_str (), ifname.size ()) < 0)
    {
        perror ("setsockopt");
        return EXIT_FAILURE;
    }

    int six = 6;
    if (setsockopt (socket_fd, SOL_SOCKET, SO_PRIORITY, &six, sizeof (int)) < 0)
    {
        perror ("setsockopt");
        return EXIT_FAILURE;
    }

    set_socket_timestamps (socket_fd);

    set_hw_timestamps (socket_fd, ifname, true);

    sockaddr_in local_sockaddr = {
        .sin_family = AF_INET,
        .sin_port = htons (PORT),
        .sin_addr = {.s_addr = INADDR_ANY},
    };
    if (bind (socket_fd, (struct sockaddr *) &local_sockaddr, sizeof (local_sockaddr)) < 0)
    {
        perror ("bind");
        return EXIT_FAILURE;
    }

#if SERVER
    server_loop (socket_fd, remote, packets, interval);
#else
    client_loop (socket_fd, remote, packets, interval);
#endif

    while (send_ts_cnt < packets)
    {
        extract_send_timestamps (socket_fd, nullptr);// the function updates send_ts_cnt
    }

    for (int i = 0; i < packets; ++i)
    {
        cout << send_ts[i] << " " << recv_ts[i] << endl;
    }

    close (socket_fd);
    return EXIT_SUCCESS;
}

int main (int argc, char **argv)
{
    if (argc < 4)
    {
        cout << "Usage: " << argv[0] << " <interface name> <remote peer ip> <num_packets>" << endl;
        return EXIT_FAILURE;
    }

    string ifname (argv[1]);
    string remote (argv[2]);
    u64 packets = stoll (argv[3]);

    send_ts.assign (packets, 0);
    recv_ts.assign (packets, 0);

    vector<int> intervals{0};
    for (auto &interval : intervals)
    {
        start_program (ifname, remote, packets, interval);
    }
}
