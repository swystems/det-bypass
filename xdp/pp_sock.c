/**
 * Pingpong experiment using AF_XDP sockets for kernel bypass.
 *
 * This program is adapted from the following file in the xdp-tutorial repository:
 * https://github.com/xdp-project/xdp-tutorial/blob/master/advanced03-AF_XDP/af_xdp_user.c
 * Credits to the original authors.
 *
 * The program functionality has been deeply modified to support the pingpong experiment and integrate with the rest of the code.
 */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>

#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <net/if.h>

#include "../common/common.h"
#include "../common/net.h"
#include "../common/persistence.h"
#include "../common/utils.h"
#include "src/args.h"
#include "src/xdp-loading.h"

#define STATS_THREAD 0
#define NUM_FRAMES 4096
#define FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE 64
#define INVALID_UMEM_FRAME UINT64_MAX
#define QUEUE_ID 0

static struct xdp_program *prog;

// Information about the XDP program.
static const char *filename = "pingpong_xsk.o";
static const char *prog_name = "xdp_xsk";
static const char *sec_name = "xdp";
static const char *pinpath = "/sys/fs/bpf/xdp_pingpong_xsk";
static const char *mapname = "xsk_map";

static const char *outfile = "pingpong_xsk.dat";
static persistence_agent_t *persistence_agent;

struct config {
    uint32_t xdp_flags;
    int ifindex;
    char *ifname;
    uint64_t iters;   // number of pingpong packet exchanges
    uint64_t interval;// interval between two pingpong packet exchanges
    uint16_t xsk_bind_flags;
    int xsk_if_queue;
    bool xsk_poll_mode;
};

struct xsk_umem_info {
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_umem *umem;
    void *buffer;
};
struct xsk_socket_info {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;

    pthread_spinlock_t xsk_client_lock;// client needs synchronization beacuse it sends and receives at the same time.

    uint64_t umem_frame_addr[NUM_FRAMES];
    uint32_t umem_frame_free;

    uint32_t outstanding_tx;
};

// File descriptor of the BPF_MAP_TYPE_XSKMAP used to receive packets from the XDP program.
int xsk_map_fd;

// Whether the program should exit.
static volatile bool global_exit;

// Initial configuration of the program. Some of the fields are overridden by the command line arguments.
struct config cfg = {
    .ifindex = 0,
    .ifname = "",

    .iters = 0,
    .interval = 0,

    .xsk_if_queue = QUEUE_ID,
    .xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_DRV_MODE,
    .xsk_bind_flags = XDP_ZEROCOPY,
    .xsk_poll_mode = false,
};

/**
 * Configure the UMEM.
 *
 * This is the shared memory area between the kernel and the user space used to exchange packets.
 * The UMEM is divided in frames of size FRAME_SIZE. Each frame works as a buffer. The kernel will write packets into
 * the frames and the user space will read them.
 *
 * The UMEM is associated with two rings: the fill ring and completion ring.
 * For more information on UMEM and the rings, see the following links:
 * - https://www.kernel.org/doc/html/next/networking/af_xdp.html#umem
 *
 * @param buffer the buffer to use for the UMEM.
 * @param size the size of the buffer.
 * @return a pointer to the UMEM information structure.
 */
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

/**
 * Allocate a frame from the UMEM. This is a pointer to a buffer of size FRAME_SIZE.
 * The frame is allocated from the UMEM frames that are not currently in use.
 * If there are no frames available, INVALID_UMEM_FRAME is returned.
 * The frame is marked as in use and must be freed with xsk_free_umem_frame.
 *
 * @param xsk the socket information structure.
 * @return the address of the allocated frame or INVALID_UMEM_FRAME if no frame is available.
 */
static uint64_t xsk_alloc_umem_frame (struct xsk_socket_info *xsk)
{
    uint64_t frame;
    if (xsk->umem_frame_free == 0)
        return INVALID_UMEM_FRAME;

    frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
    xsk->umem_frame_addr[xsk->umem_frame_free] = INVALID_UMEM_FRAME;
    return frame;
}

/**
 * Free a frame from the UMEM.
 *
 * @param xsk the socket information structure.
 * @param frame the address of the frame to free.
 */
static void xsk_free_umem_frame (struct xsk_socket_info *xsk, uint64_t frame)
{
    assert (xsk->umem_frame_free < NUM_FRAMES);

    xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

/**
 * Get the number of free frames in the UMEM.
 *
 * @param xsk the socket information structure.
 * @return the number of free frames.
 */
static uint64_t xsk_umem_free_frames (struct xsk_socket_info *xsk)
{
    return xsk->umem_frame_free;
}

/**
 * Configure the AF_XDP socket, which is used to send and receive packets.
 * The socket is associated with two rings: the receive ring and the transmit ring.
 * For more information on the rings, see the following links:
 * - https://www.kernel.org/doc/html/next/networking/af_xdp.html#rings
 *
 * @param cfg the configuration of the program.
 * @param umem the UMEM information structure.
 * @return a pointer to the socket information structure.
 */
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

    pthread_spin_init (&xsk_info->xsk_client_lock, PTHREAD_PROCESS_PRIVATE);
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

/**
 * Complete the transmission of submitted packets.
 *
 * When the userspace application sends packets, they are not immediately sent to the network interface.
 * This allows to queue multiple packets to then be sent in a single batch, increasing performance.
 * This function completes the submission of the pending packets, sending them to the network interface using
 * the sendto system call.
 * After the packets are sent, the function frees the frames used by the packets.
 *
 * @param xsk the socket information structure.
 */
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

/**
 * Submit a packet for transmission to the AF_XDP socket. If complete is true, the packet will be immediately sent.
 *
 * The submission of a packet using AF_XDP sockets happens in 4 steps:
 * - Reserve a slot in the transmission ring.
 * - If needed, reserve a frame in the UMEM and copy the packet into it.
 * - Associate the transmission ring slot with the frame.
 * - Submit the packet for transmission, notifying the kernel.
 *
 * The advantage of having this pointer-based approach in the rings is that the packet might not even need to be copied:
 * if it is already in the UMEM, the frame can be directly associated with the transmission ring slot.
 *
 * @param socket the socket information structure.
 * @param addr the address of the packet to send.
 * @param len the length of the packet.
 * @param is_umem_frame whether the packet is already in the UMEM or not.
 * @param complete whether the packet should be immediately sent.
 * @return 0 if the packet was successfully submitted, -1 otherwise.
 */
static int xsk_send_packet (struct xsk_socket_info *socket, uint64_t addr, uint32_t len, bool is_umem_frame, bool complete)
{
#if !SERVER
    pthread_spin_lock (&socket->xsk_client_lock);
#endif
    int ret;
    uint32_t tx_idx = 0;

    ret = xsk_ring_prod__reserve (&socket->tx, 1, &tx_idx);
    if (UNLIKELY (ret != 1))
    {
        /* No more transmit slots, drop the packet */
#if !SERVER
        pthread_spin_unlock (&socket->xsk_client_lock);
#endif
        return -1;
    }

    if (is_umem_frame)
    {
        xsk_ring_prod__tx_desc (&socket->tx, tx_idx)->addr = addr;
    }
    else
    {
        xsk_ring_prod__tx_desc (&socket->tx, tx_idx)->addr = xsk_alloc_umem_frame (socket);
        memcpy (xsk_umem__get_data (socket->umem->buffer, xsk_ring_prod__tx_desc (&socket->tx, tx_idx)->addr), (void *) addr, len);
    }
    xsk_ring_prod__tx_desc (&socket->tx, tx_idx)->len = len;
    xsk_ring_prod__submit (&socket->tx, 1);
    socket->outstanding_tx++;

    if (complete)
    {
        complete_tx (socket);
    }
#if !SERVER
    pthread_spin_unlock (&socket->xsk_client_lock);
#endif

    return 0;
}

/**
 * Process the packet located at the given address.
 *
 * @param xsk the socket information structure.
 * @param addr the UMEM address of the packet.
 * @param len the length of the packet.
 * @return true if the packet will be sent back, i.e. the UMEM frame must be kept; false otherwise.
 */
static bool process_packet (struct xsk_socket_info *xsk,
                            uint64_t addr, uint32_t len)
{
    uint64_t receive_timestamp = get_time_ns ();
    uint8_t *pkt = xsk_umem__get_data (xsk->umem->buffer, addr);

    if (len < sizeof (struct ethhdr) + sizeof (struct iphdr) + sizeof (struct pingpong_payload))
    {
        LOG (stderr, "Received packet is too small\n");
        return false;
    }

    struct ethhdr *eth = (struct ethhdr *) pkt;
    struct pingpong_payload *payload = packet_payload ((char *) pkt);

    if (eth->h_proto != htons (ETH_P_PINGPONG))
    {
        LOG (stderr, "Received non-pingpong packet\n");
        return false;
    }

    if (payload->id >= cfg.iters)
        global_exit = true;

#if SERVER
    struct iphdr *ip = (struct iphdr *) (eth + 1);
    uint8_t tmp_mac[ETH_ALEN];
    uint32_t tmp_ip;

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
    xsk_send_packet (xsk, addr, len, true, false);
    return true;// the packet is queued for transmission, so we must keep the frame
#else
    payload->ts[3] = receive_timestamp;
    persistence_agent->write (persistence_agent, payload);
    return false;// the packet has no reason to be kept
#endif
}

/**
 * Handle the reception of packets.
 *
 * Check if there is any available packet to be processed by checking the receive ring.
 * If there are packets available, process them.
 *
 * @param xsk the socket information structure.
 */
static void handle_receive_packets (struct xsk_socket_info *xsk)
{
    //START_TIMER ();
    unsigned int rcvd, stock_frames, i;
    uint32_t idx_rx = 0, idx_fq = 0;

    rcvd = xsk_ring_cons__peek (&xsk->rx, RX_BATCH_SIZE, &idx_rx);

    if (!rcvd)
    {
        return;
    }

#if !SERVER
    pthread_spin_lock (&xsk->xsk_client_lock);
#endif

    /*
     * Not sure about this instruction and the if block.
     * Without it, the send stops after 2048 packets, i.e. it must be related about the available
     * number of frames in the UMEM/elements in the rings.
     *
     * If only the libxdp functions were a bit more documented...
     */
    stock_frames = xsk_prod_nb_free (&xsk->umem->fq,
                                     xsk_umem_free_frames (xsk));

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
    }

    xsk_ring_cons__release (&xsk->rx, rcvd);

    /* Do we need to wake up the kernel for transmission */
    complete_tx (xsk);

#if !SERVER
    pthread_spin_unlock (&xsk->xsk_client_lock);
#endif

    //STOP_TIMER ();
}

/**
 * Keeps polling for new packets and, if available, process them.
 * This function can either use a busy-wait or a blocking poll.
 * The busy-wait is the default behavior.
 *
 * @param cfg the configuration of the program.
 * @param xsk_socket the socket information structure.
 */
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

/**
 * Send a pingpong packet to the server.
 * This function is called by the send thread every cfg.interval nanoseconds.
 *
 * @param buf the buffer containing the packet to send.
 * @param buf_len the length of the packet.
 * @param addr the socket address of the server.
 * @param aux the socket information structure.
 * @return 0 if the packet was successfully sent, -1 otherwise.
 */
int client_send_pp_packet (char *buf, const uint64_t packet_id, struct sockaddr_ll *addr __unused, void *aux)
{
    struct iphdr *ip = (struct iphdr *) (buf + sizeof (struct ethhdr));
    ip->id = htons (packet_id);
    struct pingpong_payload *payload = packet_payload (buf);
    *payload = new_pingpong_payload (packet_id);
    payload->ts[0] = get_time_ns ();

    struct xsk_socket_info *socket = (struct xsk_socket_info *) aux;

    int ret = xsk_send_packet (socket, (uint64_t) buf, PACKET_SIZE, false, true);
    if (ret)
    {
        LOG (stderr, "Failed to send packet\n");
        return -1;
    }

    return 0;
}

/**
 * Initialize the client functionality, i.e. start sending pingpong packets to the server.
 *
 * @param cfg the configuration of the program.
 * @param socket the socket information structure.
 * @param src_mac the MAC address of the client.
 * @param dest_mac the MAC address of the server.
 * @param src_ip the IP address of the client.
 * @param dest_ip the IP address of the server.
 */
void initialize_client (const struct config *cfg, struct xsk_socket_info *socket, uint8_t *src_mac, uint8_t *dest_mac, uint32_t *src_ip, uint32_t *dest_ip)
{
    const int ifindex = cfg->ifindex;
    char *base_packet = malloc (PACKET_SIZE);
    build_base_packet (base_packet, src_mac, dest_mac, *src_ip, *dest_ip);

    struct sockaddr_ll sock_addr = build_sockaddr (ifindex, dest_mac);

    start_sending_packets (cfg->iters, cfg->interval, base_packet, &sock_addr, client_send_pp_packet, (void *) socket);
}

int xsk_cleanup (struct xsk_socket_info *xsk)
{
    xsk_socket__delete (xsk->xsk);
    xsk_umem__delete (xsk->umem->umem);
#if !SERVER
    pthread_spin_destroy (&xsk->xsk_client_lock);
#endif
    free (xsk->umem);
    free (xsk);
    return 0;
}

/**
 * Handle the SIGINT signal, which is sent when the user presses Ctrl+C.
 *
 * @param sig the signal number.
 */
void interrupt_handler (int sig __unused)
{
    global_exit = true;
}

int main (int argc __unused, char **argv __unused)
{
    int ret;
    void *packet_buffer;
    uint64_t packet_buffer_size;
    struct xsk_umem_info *umem;
    struct xsk_socket_info *xsk_socket;

    bool remove = false;

    cfg.ifname = NULL;
    cfg.iters = 0;
    char *server_ip = NULL;
    cfg.interval = 0;

#if SERVER
    if (!xdp_parse_args (argc, argv, &cfg.ifname, &remove, &cfg.iters))
    {
        xdp_print_usage (argv[0]);
        return EXIT_FAILURE;
    }
#else
    uint32_t persistence_flags = PERSISTENCE_M_ALL_TIMESTAMPS;

    if (!xdp_parse_args (argc, argv, &cfg.ifname, &remove, &cfg.iters, &cfg.interval, &server_ip, &persistence_flags))
    {
        xdp_print_usage (argv[0]);
        return EXIT_FAILURE;
    }
#endif

    cfg.ifindex = if_nametoindex (cfg.ifname);

    if (remove)
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

    struct bpf_object *obj = read_xdp_file (filename);
    if (obj == NULL)
    {
        fprintf (stderr, "ERR: loading file: %s\n", filename);
        return EXIT_FAILURE;
    }

    detach_xdp (obj, prog_name, cfg.ifindex, pinpath);

    uint8_t src_mac[ETH_ALEN];
    uint32_t src_ip;
    uint8_t dest_mac[ETH_ALEN];
    uint32_t dest_ip;
    exchange_eth_ip_addresses (cfg.ifindex, server_ip, SERVER, src_mac, dest_mac, &src_ip, &dest_ip);

    signal (SIGINT, interrupt_handler);

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

    START_TIMER ();
    fprintf (stdout, "Starting experiment\n");
    fflush (stdout);

#if !SERVER
    persistence_agent = persistence_init (outfile, persistence_flags, &cfg.interval);
    initialize_client (&cfg, xsk_socket, src_mac, dest_mac, &src_ip, &dest_ip);
#endif

    /* Receive and count packets than drop them */
    rx_and_process (&cfg, xsk_socket);

    STOP_TIMER ();
    uint64_t time_taken = (__end - __start) / 1000000LL;
    fprintf (stdout, "Experiment finished in %lu milliseconds.\n", time_taken);

    /* Cleanup */
#if !SERVER
    pthread_cancel (get_sender_thread ());
    pthread_join (get_sender_thread (), NULL);
#endif
    xsk_cleanup (xsk_socket);

    if (persistence_agent)
        persistence_agent->close (persistence_agent);

    bpf_xdp_detach (cfg.ifindex, XDP_FLAGS_DRV_MODE, 0);

    return EXIT_SUCCESS;
}