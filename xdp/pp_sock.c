#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>

#include <bpf/bpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include <arpa/inet.h>
#include <linux/icmpv6.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/ipv6.h>
#include <net/if.h>

#include "common.h"
#include "src/net.h"
#include "src/utils.h"
#include "src/xdp-loading.h"

#define STATS_THREAD 0
#define NUM_FRAMES 4096
#define FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE 64
#define INVALID_UMEM_FRAME UINT64_MAX
#define QUEUE_ID 0

static struct xdp_program *prog;
static const char *filename = "pingpong_xsk.o";
static const char *prog_name = "xdp_xsk";
static const char *sec_name = "xdp";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong_xsk";
static const char *mapname = "xsk_map";

static bool is_server = false;

//static const char *outfile = "pingpong.dat";

void usage (char *prog)
{
    fprintf (stderr, "Usage: %s <ifname> <action> [extra arguments]\n", prog);
    fprintf (stderr, "Actions:\n");
    fprintf (stderr, "\t- start: start the pingpong experiment\n");
    fprintf (stderr, "\t         on the server: %s <ifname> start <# of packets>\n", prog);
    fprintf (stderr, "\t         on the client: %s <ifname> start <# of packets> <packets interval (ns)> <server IP>\n", prog);
    fprintf (stderr, "\t- remove: remove XDP program\n");
}

int xsk_map_fd;

struct config {
    __u32 xdp_flags;
    int ifindex;
    char *ifname;
    uint32_t iters;        // number of pingpong packet exchanges
    uint64_t interval;// interval between two pingpong packet exchanges
    __u16 xsk_bind_flags;
    int xsk_if_queue;
    bool xsk_poll_mode;
};

struct config cfg = {
    .ifindex = 0,
    .ifname = "",

    .iters = 0,
    .interval = 0,

    .xsk_if_queue = QUEUE_ID,
    .xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_DRV_MODE,
    .xsk_bind_flags = 0,
    .xsk_poll_mode = false,
};

struct xsk_umem_info {
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_umem *umem;
    void *buffer;
};
struct stats_record {
    uint64_t timestamp;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
};
struct xsk_socket_info {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;

    uint64_t umem_frame_addr[NUM_FRAMES];
    uint32_t umem_frame_free;

    uint32_t outstanding_tx;

    struct stats_record stats;
    struct stats_record prev_stats;
};

static inline __u32 xsk_ring_prod__free (struct xsk_ring_prod *r)
{
    r->cached_cons = *r->consumer + r->size;
    return r->cached_cons - r->cached_prod;
}

static volatile bool global_exit;

static struct xsk_umem_info *configure_xsk_umem (void *buffer, uint64_t size)
{
    struct xsk_umem_info *umem;
    int ret;

    umem = calloc (1, sizeof (*umem));
    if (!umem)
        return NULL;

    ret = xsk_umem__create (&umem->umem, buffer, size, &umem->fq, &umem->cq,
                            NULL);
    if (ret)
    {
        errno = -ret;
        return NULL;
    }

    umem->buffer = buffer;
    return umem;
}

static uint64_t xsk_alloc_umem_frame (struct xsk_socket_info *xsk)
{
    uint64_t frame;
    if (xsk->umem_frame_free == 0)
        return INVALID_UMEM_FRAME;

    frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
    xsk->umem_frame_addr[xsk->umem_frame_free] = INVALID_UMEM_FRAME;
    return frame;
}

static void xsk_free_umem_frame (struct xsk_socket_info *xsk, uint64_t frame)
{
    assert (xsk->umem_frame_free < NUM_FRAMES);

    xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

static __unused uint64_t xsk_umem_free_frames (struct xsk_socket_info *xsk)
{
    return xsk->umem_frame_free;
}

static struct xsk_socket_info *xsk_configure_socket (struct config *cfg,
                                                     struct xsk_umem_info *umem)
{
    struct xsk_socket_config xsk_cfg;
    struct xsk_socket_info *xsk_info;
    uint32_t idx;
    int i;
    int ret;

    xsk_info = calloc (1, sizeof (*xsk_info));
    if (!xsk_info)
        return NULL;

    xsk_info->umem = umem;
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.xdp_flags = cfg->xdp_flags;
    xsk_cfg.bind_flags = cfg->xsk_bind_flags;
    xsk_cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
    ret = xsk_socket__create (&xsk_info->xsk, cfg->ifname,
                              cfg->xsk_if_queue, umem->umem, &xsk_info->rx,
                              &xsk_info->tx, &xsk_cfg);
    if (ret)
        goto error_exit;

    ret = xsk_socket__update_xskmap (xsk_info->xsk, xsk_map_fd);
    if (ret)
        goto error_exit;

    /* Initialize umem frame allocation */
    for (i = 0; i < NUM_FRAMES; i++)
        xsk_info->umem_frame_addr[i] = i * FRAME_SIZE;

    xsk_info->umem_frame_free = NUM_FRAMES;

    /* Stuff the receive path with buffers, we assume we have enough */
    ret = xsk_ring_prod__reserve (&xsk_info->umem->fq,
                                  XSK_RING_PROD__DEFAULT_NUM_DESCS,
                                  &idx);

    if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS)
        goto error_exit;

    for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i++)
        *xsk_ring_prod__fill_addr (&xsk_info->umem->fq, idx++) =
            xsk_alloc_umem_frame (xsk_info);

    xsk_ring_prod__submit (&xsk_info->umem->fq,
                           XSK_RING_PROD__DEFAULT_NUM_DESCS);

    return xsk_info;

error_exit:
    errno = -ret;
    return NULL;
}

static void complete_tx (struct xsk_socket_info *xsk)
{
    unsigned int completed;
    uint32_t idx_cq;

    if (!xsk->outstanding_tx)
        return;

    sendto (xsk_socket__fd (xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);

    /* Collect/free completed TX buffers */
    completed = xsk_ring_cons__peek (&xsk->umem->cq,
                                     XSK_RING_CONS__DEFAULT_NUM_DESCS,
                                     &idx_cq);

    if (completed > 0)
    {
        for (unsigned int i = 0; i < completed; i++)
            xsk_free_umem_frame (xsk,
                                 *xsk_ring_cons__comp_addr (&xsk->umem->cq,
                                                            idx_cq++));

        xsk_ring_cons__release (&xsk->umem->cq, completed);
        xsk->outstanding_tx -= completed < xsk->outstanding_tx ? completed : xsk->outstanding_tx;
    }
}

static bool process_packet (struct xsk_socket_info *xsk,
                            uint64_t addr, uint32_t len)
{
    uint64_t receive_timestamp = get_time_ns ();
    uint8_t *pkt = xsk_umem__get_data (xsk->umem->buffer, addr);

    int ret;
    uint32_t tx_idx = 0;
    uint8_t tmp_mac[ETH_ALEN];
    uint32_t tmp_ip;

    if (len < sizeof (struct ethhdr) + sizeof (struct iphdr) + sizeof (struct pingpong_payload))
    {
        LOG (stderr, "Received packet is too small\n");
        return false;
    }

    struct ethhdr *eth = (struct ethhdr *) pkt;
    struct iphdr *ip = (struct iphdr *) (eth + 1);
    struct pingpong_payload *payload = packet_payload ((char *) pkt);

    if (eth->h_proto != htons (ETH_P_PINGPONG))
    {
        LOG (stderr, "Received non-pingpong packet\n");
        return false;
    }

    if (payload->id >= cfg.iters)
        global_exit = true;

    if (is_server)
    {
        //LOG (stdout, "Received ping with id %d, sending pong\n", payload->id);
        // set ts[1] with arrival timestamp and ts[2] with send timestamp
        payload->ts[1] = receive_timestamp;

        // swap mac and ip addresses
        memcpy (tmp_mac, eth->h_dest, ETH_ALEN);
        memcpy (eth->h_dest, eth->h_source, ETH_ALEN);
        memcpy (eth->h_source, tmp_mac, ETH_ALEN);

        tmp_ip = ip->daddr;
        ip->daddr = ip->saddr;
        ip->saddr = tmp_ip;

        payload->ts[2] = get_time_ns ();

        /* Here we sent the packet out of the receive port. Note that
     * we allocate one entry and schedule it. Your design would be
     * faster if you do batch processing/transmission */

        ret = xsk_ring_prod__reserve (&xsk->tx, 1, &tx_idx);
        if (ret != 1)
        {
            /* No more transmit slots, drop the packet */
            return false;
        }
        xsk_ring_prod__tx_desc (&xsk->tx, tx_idx)->addr = addr;
        xsk_ring_prod__tx_desc (&xsk->tx, tx_idx)->len = len;

        xsk_ring_prod__submit (&xsk->tx, 1);
        xsk->outstanding_tx++;

        xsk->stats.tx_bytes += len;
        xsk->stats.tx_packets++;
        return true;
    }
    else
    {
        payload->ts[3] = receive_timestamp;
        //        LOG (stdout, "%d\n", payload->id);
        LOG (stdout, "Packet %d: %llu %llu %llu %llu\n", payload->id, payload->ts[0], payload->ts[1], payload->ts[2], payload->ts[3]);
        return false;
    }
}

static void handle_receive_packets (struct xsk_socket_info *xsk)
{
    //START_TIMER ();
    unsigned int rcvd, stock_frames, i;
    uint32_t idx_rx = 0, idx_fq = 0;

    rcvd = xsk_ring_cons__peek (&xsk->rx, RX_BATCH_SIZE, &idx_rx);
    if (!rcvd)
        return;

    /* Stuff the ring with as much frames as possible */
    stock_frames = xsk_prod_nb_free (&xsk->umem->fq,
                                     1);

    if (stock_frames > 0)
    {

        unsigned int ret = xsk_ring_prod__reserve (&xsk->umem->fq, stock_frames,
                                                   &idx_fq);

        /* This should not happen, but just in case */
        while (ret != stock_frames)
            ret = xsk_ring_prod__reserve (&xsk->umem->fq, rcvd,
                                          &idx_fq);

        for (i = 0; i < stock_frames; i++)
            *xsk_ring_prod__fill_addr (&xsk->umem->fq, idx_fq++) =
                xsk_alloc_umem_frame (xsk);

        xsk_ring_prod__submit (&xsk->umem->fq, stock_frames);
    }

    /* Process received packets */
    for (i = 0; i < rcvd; i++)
    {
        uint64_t addr = xsk_ring_cons__rx_desc (&xsk->rx, idx_rx)->addr;
        uint32_t len = xsk_ring_cons__rx_desc (&xsk->rx, idx_rx++)->len;

        if (!process_packet (xsk, addr, len))
            xsk_free_umem_frame (xsk, addr);

        xsk->stats.rx_bytes += len;
    }

    xsk_ring_cons__release (&xsk->rx, rcvd);
    xsk->stats.rx_packets += rcvd;

    /* Do we need to wake up the kernel for transmission */
    complete_tx (xsk);

    //STOP_TIMER ();
}

static void rx_and_process (struct config *cfg,
                            struct xsk_socket_info *xsk_socket)
{
    struct pollfd fds[2];
    int ret, nfds = 1;

    memset (fds, 0, sizeof (fds));
    fds[0].fd = xsk_socket__fd (xsk_socket->xsk);
    fds[0].events = POLLIN;

    while (!global_exit)
    {
        if (cfg->xsk_poll_mode)
        {
            ret = poll (fds, nfds, -1);
            if (ret <= 0 || ret > 1)
                continue;
        }
        handle_receive_packets (xsk_socket);
    }
}

#if STATS_THREAD
static double calc_period (struct stats_record *r, struct stats_record *p)
{
    double period_ = 0;
    __u64 period = 0;

    period = r->timestamp - p->timestamp;
    if (period > 0)
        period_ = ((double) period / 1e9);

    return period_;
}

pthread_t stats_poll_thread;
static void stats_print (struct stats_record *stats_rec,
                         struct stats_record *stats_prev)
{
    uint64_t packets, bytes;
    double period;
    double pps; /* packets per sec */
    double bps; /* bits per sec */

    char *fmt = "%-12s %'11lld pkts (%'10.0f pps)"
                " %'11lld Kbytes (%'6.0f Mbits/s)"
                " period:%f\n";

    period = calc_period (stats_rec, stats_prev);
    if (period == 0)
        period = 1;

    packets = stats_rec->rx_packets - stats_prev->rx_packets;
    pps = packets / period;

    bytes = stats_rec->rx_bytes - stats_prev->rx_bytes;
    bps = (bytes * 8) / period / 1000000;

    printf (fmt, "AF_XDP RX:", stats_rec->rx_packets, pps,
            stats_rec->rx_bytes / 1000, bps,
            period);

    packets = stats_rec->tx_packets - stats_prev->tx_packets;
    pps = packets / period;

    bytes = stats_rec->tx_bytes - stats_prev->tx_bytes;
    bps = (bytes * 8) / period / 1000000;

    printf (fmt, "       TX:", stats_rec->tx_packets, pps,
            stats_rec->tx_bytes / 1000, bps,
            period);

    printf ("\n");
}
static void *stats_poll (void *arg)
{
    unsigned int interval = 2;
    struct xsk_socket_info *xsk = arg;
    static struct stats_record previous_stats = {0};

    previous_stats.timestamp = get_time_ns ();

    /* Trick to pretty printf with thousands separators use %' */
    setlocale (LC_NUMERIC, "en_US");

    while (!global_exit)
    {
        sleep (interval);
        xsk->stats.timestamp = get_time_ns ();
        stats_print (&xsk->stats, &previous_stats);
        previous_stats = xsk->stats;
    }
    return NULL;
}
#endif

//static void exit_application (int signal __unused)
//{
//    global_exit = true;
//
//    signal = signal; /* For compiler warning */
//}

int send_single_packet (const char *buf, const int buf_len, struct sockaddr_ll *addr __unused, void *aux)
{
    struct xsk_socket_info *socket = (struct xsk_socket_info *) aux;

    int ret;
    uint32_t tx_idx = 0;

    ret = xsk_ring_prod__reserve (&socket->tx, 1, &tx_idx);
    if (ret != 1)
    {
        /* No more transmit slots, drop the packet */
        return -1;
    }

    xsk_ring_prod__tx_desc (&socket->tx, tx_idx)->addr = xsk_alloc_umem_frame (socket);
    xsk_ring_prod__tx_desc (&socket->tx, tx_idx)->len = buf_len;
    memcpy (xsk_umem__get_data (socket->umem->buffer, xsk_ring_prod__tx_desc (&socket->tx, tx_idx)->addr), buf, buf_len);
    xsk_ring_prod__submit (&socket->tx, 1);
    socket->outstanding_tx++;

    socket->stats.tx_bytes += buf_len;
    socket->stats.tx_packets++;

    complete_tx (socket);

    //LOG (stdout, "Sent packet %d\n", packet_payload (buf)->id);

    return 0;
}

void initialize_client (const struct config *cfg, struct xsk_socket_info *socket, uint8_t *src_mac, uint8_t *dest_mac, uint32_t *src_ip, uint32_t *dest_ip)
{
    const int ifindex = cfg->ifindex;
    char *base_packet = malloc (PACKET_SIZE);
    build_base_packet (base_packet, src_mac, dest_mac, *src_ip, *dest_ip);

    struct sockaddr_ll sock_addr = build_sockaddr (ifindex, dest_mac);

    start_sending_packets (cfg->iters, cfg->interval, base_packet, &sock_addr, send_single_packet, (void *) socket);
}

int main (int argc __unused, char **argv __unused)
{
    int ret;
    void *packet_buffer;
    uint64_t packet_buffer_size;
    struct xsk_umem_info *umem;
    struct xsk_socket_info *xsk_socket;

    /* Global shutdown handler */
    //signal (SIGINT, exit_application);

    if (argc < 3)
    {
        usage (argv[0]);
        return EXIT_FAILURE;
    }

    cfg.ifname = argv[1];
    cfg.ifindex = if_nametoindex (cfg.ifname);
    if (cfg.ifindex == 0)
    {
        fprintf (stderr, "ERROR: Interface %s not found\n", cfg.ifname);
        return EXIT_FAILURE;
    }

    if (strcmp (argv[2], "remove") == 0)
    {
        struct bpf_object *obj = read_xdp_file (filename);
        if (obj == NULL)
        {
            fprintf (stderr, "ERR: loading file: %s\n", filename);
            return EXIT_FAILURE;
        }

        detach_xdp (obj, prog_name, cfg.ifindex, pinpath);
        return EXIT_SUCCESS;
    }
    else if (strcmp (argv[2], "start") != 0)
    {
        usage (argv[0]);
        return EXIT_FAILURE;
    }

    if (argc != 4 && argc != 6)
    {
        usage (argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 6)
    {
        is_server = false;
    }
    else
    {
        is_server = true;
    }

    cfg.iters = atoi (argv[3]);
    char *server_ip = NULL;
    if (!is_server)
    {
        cfg.interval = atol (argv[4]);
        server_ip = argv[5];
    }

    uint8_t src_mac[ETH_ALEN];
    uint32_t src_ip;
    uint8_t dest_mac[ETH_ALEN];
    uint32_t dest_ip;
    exchange_addresses (cfg.ifindex, server_ip, is_server, src_mac, dest_mac, &src_ip, &dest_ip);

    /* Required option */
    if (cfg.ifindex == -1)
    {
        fprintf (stderr, "ERROR: Required option --dev missing\n\n");
        return EXIT_FAILURE;
    }

    struct bpf_object *obj = read_xdp_file (filename);
    if (obj == NULL)
    {
        fprintf (stderr, "ERR: loading file: %s\n", filename);
        return EXIT_FAILURE;
    }

    detach_xdp (obj, prog_name, cfg.ifindex, pinpath);
    obj = read_xdp_file (filename);
    if (obj == NULL)
    {
        fprintf (stderr, "ERR: loading file: %s\n", filename);
        return EXIT_FAILURE;
    }

    prog = xdp_program__from_bpf_obj (obj, sec_name);

    // attach the pingpong XDP program
    ret = attach_xdp (obj, prog_name, cfg.ifindex, pinpath);
    if (ret)
    {
        fprintf (stderr, "ERR: attaching program failed\n");
        return EXIT_FAILURE;
    }

    xsk_map_fd = bpf_object__find_map_fd_by_name (obj, mapname);

    /* Allow unlimited locking of memory, so all memory needed for packet
	 * buffers can be locked.
	 */
    struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
    if (setrlimit (RLIMIT_MEMLOCK, &rlim))
    {
        fprintf (stderr, "ERROR: setrlimit(RLIMIT_MEMLOCK) \"%s\"\n",
                 strerror (errno));
        exit (EXIT_FAILURE);
    }

    /* Allocate memory for NUM_FRAMES of the default XDP frame size */
    packet_buffer_size = NUM_FRAMES * FRAME_SIZE;
    if (posix_memalign (&packet_buffer,
                        getpagesize (), /* PAGE_SIZE aligned */
                        packet_buffer_size))
    {
        fprintf (stderr, "ERROR: Can't allocate buffer memory \"%s\"\n",
                 strerror (errno));
        exit (EXIT_FAILURE);
    }

    /* Initialize shared packet_buffer for umem usage */
    umem = configure_xsk_umem (packet_buffer, packet_buffer_size);
    if (umem == NULL)
    {
        fprintf (stderr, "ERROR: Can't create umem \"%s\"\n",
                 strerror (errno));
        exit (EXIT_FAILURE);
    }

    /* Open and configure the AF_XDP (xsk) socket */
    xsk_socket = xsk_configure_socket (&cfg, umem);
    if (xsk_socket == NULL)
    {
        fprintf (stderr, "ERROR: Can't setup AF_XDP socket \"%s\"\n",
                 strerror (errno));
        exit (EXIT_FAILURE);
    }

    /* Start thread to do statistics display */
#if STATS_THREAD
    ret = pthread_create (&stats_poll_thread, NULL, stats_poll,
                          xsk_socket);
    if (ret)
    {
        fprintf (stderr, "ERROR: Failed creating statistics thread "
                         "\"%s\"\n",
                 strerror (errno));
        exit (EXIT_FAILURE);
    }
#endif

    if (!is_server)
    {
        initialize_client (&cfg, xsk_socket, src_mac, dest_mac, &src_ip, &dest_ip);
    }

    /* Receive and count packets than drop them */
    rx_and_process (&cfg, xsk_socket);

    /* Cleanup */
    xsk_socket__delete (xsk_socket->xsk);
    xsk_umem__delete (umem->umem);

    return EXIT_SUCCESS;
}