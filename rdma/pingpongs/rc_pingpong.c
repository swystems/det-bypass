/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <getopt.h>
#include <inttypes.h>
#include <malloc.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ccan/minmax.h"
#include "pingpong.h"

/**
 * The ID of the receive and send work requests.
 */
enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};

// Size of the pages in the system.
static int page_size;
// On Demand Paging (ODP): a technique to reduce the latency of the memory access.
// Without ODP, all the pages are allocated in memory when registered.
// With Explicit ODP, the program must register the pages but they are dynamically loaded when needed.
// With Implicit ODP, the program can define the memory region with a "key", and the actual registration and loading
// of the pages is done by the HCA (Host Channel Adapter).
//
// Reference: https://enterprise-support.nvidia.com/s/article/understanding-on-demand-paging--odp-x
static int use_odp;
static int implicit_odp;

// Whether to prefetch the ODP memory region.
static int prefetch_mr;

// Whether to do timestamping at NIC-time.
static int use_ts;

// Whether the received buffer should be validated for correctness.
// This is simply done by memsetting the buffer to a specific value by the sender, and checked for the same values
// by the receiver.
static int validate_buf;

// Whether to use Device Memory (DM).
// Device Memory is a memory region that is directly accessible by the devices, without the need of the CPU.
// The difference with Memory Region (MR) is that the MR is managed by the CPU, while the DM is managed by the devices(?)
static int use_dm;

// Whether to use the new send API.
// The new send API is a new API that allows to send multiple messages with a single call.
static int use_new_send;

struct pingpong_data *pp_data;

struct pingpong_context {
    struct ibv_context *context;
    struct ibv_comp_channel *channel;

    // Protection Domain (PD) of the app.
    // Reference: https://www.ibm.com/docs/en/sdk-java-technology/8?topic=jverbs-protectiondomain
    struct ibv_pd *pd;

    // Memory Region (MR) to be used by the devices to write and read memory.
    // Reference: https://www.rdmamojo.com/2012/09/07/ibv_reg_mr/
    struct ibv_mr *mr;

    // Device Memory (DM) idk???
    struct ibv_dm *dm;

    union {
        // Completion queue (CQ)
        // Reference: https://man7.org/linux/man-pages/man3/ibv_create_cq.3.html
        struct ibv_cq *cq;

        // Extended completion queue (CQ_EX)
        // Reference: https://man7.org/linux/man-pages/man3/ibv_create_cq_ex.3.html
        // This struct is generated when the timestamps are taken at NIC-time.
        struct ibv_cq_ex *cq_ex;
    } cq_s;

    // Queue Pair (QP): a pair of send and receive queue.
    struct ibv_qp *qp;

    // Extended Queue Pair (QP_EX)
    // As far as I understand it, the major difference between normal structs and extended structs is only the
    // fact that extended structs contain pointers to functions (simil-OOP).
    struct ibv_qp_ex *qpx;

    // Buffer to be used as memory region.
    // My guess is that the buffer used in ibv_mr is not managed, but it must be created and freed autonomously.
    union {
        char *buf;
        struct pingpong_payload *pp_buf;
    };

    int size;

    int send_flags;

    // Number of receives to post at a time.
    // I understand the goal, I do not understand the naming (depth?).
    int rx_depth;

    // The next message expected (I guess?) i.e. ping or pong.
    int pending;

    struct ibv_port_attr portinfo;

    uint64_t completion_timestamp_mask;
};

/**
 * Retrieve the Completion Queue from the context.
 * This is always the normal Completion Queue, unless timestamping is not enabled;
 * in that case, the extended completion queue is taken, casted and then returned.
 *
 * @param ctx The pingpong context.
 * @return The completion queue.
 */
static struct ibv_cq *
pp_cq (struct pingpong_context *ctx)
{
    return use_ts ? ibv_cq_ex_to_cq (ctx->cq_s.cq_ex) : ctx->cq_s.cq;
}

/**
 * Destination of a pingpong packet.
 */
struct pingpong_dest {
    // LID (Layer 2): InfiniBand version of MAC address(?)
    int lid;

    // QPN (Queue Pair Number): the ID of the queue pair the packet is destinated to.
    // In Reliable Channel, each queue pair is linked with another queue pair.
    int qpn;

    // PSN (Packet Sequence Number): the ID of the packet.
    // In a Reliable Channel, the PSN is an incrementing number identifying the packet; at receiver-side, packets are
    // ordered based on this number.
    int psn;

    // GID (Global IDentifier): 128-bit IPv6 address withing InfiniBand networks.
    // In RoCE, it is associated in a GID table with the IP of the host in the network.
    union ibv_gid gid;
};

/**
 * Connect the application with the destination.
 *
 * @param ctx the context of the pingpong app
 * @param port the port of this host
 * @param my_psn the
 * @param mtu
 * @param sl
 * @param dest
 * @param sgid_idx
 * @return
 */
static int
pp_connect_ctx (struct pingpong_context *ctx, int port, int my_psn, enum ibv_mtu mtu, int sl, struct pingpong_dest *dest,
                int sgid_idx)
{
    struct ibv_qp_attr attr = {.qp_state = IBV_QPS_RTR, .path_mtu = mtu, .dest_qp_num = dest->qpn, .rq_psn = dest->psn, .max_dest_rd_atomic = 1, .min_rnr_timer = 12, .ah_attr = {.is_global = 0, .dlid = dest->lid, .sl = sl, .src_path_bits = 0, .port_num = port}};

    if (dest->gid.global.interface_id)
    {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.dgid = dest->gid;
        attr.ah_attr.grh.sgid_index = sgid_idx;
    }
    if (ibv_modify_qp (ctx->qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
    {
        fprintf (stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = my_psn;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp (ctx->qp, &attr,
                       IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
    {
        fprintf (stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    return 0;
}

/**
 * As a client machine, exchange the destination information with the specified server.
 * @param servername the ip address of the server
 * @param port the port of the server
 * @param my_dest the local destination information
 * @return the destination information of the server
 */
static struct pingpong_dest *
pp_client_exch_dest (const char *servername, int port, const struct pingpong_dest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    // Contains the port number as a string. Allocation happens in `asprintf`.
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

    if (asprintf (&service, "%d", port) < 0)
        return NULL;

    // Get the socket addresses of the server.
    n = getaddrinfo (servername, service, &hints, &res);

    if (n < 0)
    {
        fprintf (stderr, "%s for %s:%d\n", gai_strerror (n), servername, port);
        free (service);
        return NULL;
    }

    // Try to connect to all the sockets until one succeeds.
    for (t = res; t; t = t->ai_next)
    {
        sockfd = socket (t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0)
        {
            if (!connect (sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close (sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo (res);
    free (service);

    // If no connection was established, return NULL.
    if (sockfd < 0)
    {
        fprintf (stderr, "Couldn't connect to %s:%d\n", servername, port);
        return NULL;
    }

    // Format the GID to send it to the server as a string.
    gid_to_wire_gid (&my_dest->gid, gid);
    // Generate the message to send to the server with the local address.
    sprintf (msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);

    // Send the message to the server.
    if (write (sockfd, msg, sizeof msg) != sizeof msg)
    {
        fprintf (stderr, "Couldn't send local address\n");
        goto out;
    }

    // Receive the message from the server.
    if (read (sockfd, msg, sizeof msg) != sizeof msg || write (sockfd, "done", sizeof "done") != sizeof "done")
    {
        perror ("client read/write");
        fprintf (stderr, "Couldn't read/write remote address\n");
        goto out;
    }

    // Allocated the struct for the remote address.
    rem_dest = malloc (sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    // Parse the message received from the server.
    sscanf (msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    // Convert the GID string to a GID struct.
    wire_gid_to_gid (gid, &rem_dest->gid);

out:
    close (sockfd);
    return rem_dest;
}

/**
 * As a server machine, exchange the destination information with any connecting client.
 * @param ctx the application context
 * @param ib_port the port of the InfiniBand device
 * @param mtu the MTU of the connection
 * @param port the port for the server to listen to
 * @param sl the service level
 * @param my_dest the local destination information
 * @param sgid_idx the index of the GID
 * @return
 */
static struct pingpong_dest *
pp_server_exch_dest (struct pingpong_context *ctx, int ib_port, enum ibv_mtu mtu, int port, int sl,
                     const struct pingpong_dest *my_dest, int sgid_idx)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {.ai_flags = AI_PASSIVE, .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1, connfd;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

    if (asprintf (&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo (NULL, service, &hints, &res);

    if (n < 0)
    {
        fprintf (stderr, "%s for port %d\n", gai_strerror (n), port);
        free (service);
        return NULL;
    }

    // Try to bind to all the sockets until one succeeds.
    for (t = res; t; t = t->ai_next)
    {
        sockfd = socket (t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0)
        {
            n = 1;

            setsockopt (sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

            if (!bind (sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close (sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo (res);
    free (service);

    // If it was not possible to bind to any socket, abort.
    if (sockfd < 0)
    {
        fprintf (stderr, "Couldn't listen to port %d\n", port);
        return NULL;
    }

    // Start listening to the socket, waiting for client connections.
    listen (sockfd, 1);

    // Accept the connection from the client.
    connfd = accept (sockfd, NULL, NULL);

    // Close the listening socket, we got the client.
    close (sockfd);
    if (connfd < 0)
    {
        fprintf (stderr, "accept() failed\n");
        return NULL;
    }

    // Read the message from the client, containing its local address.
    n = read (connfd, msg, sizeof msg);
    if (n != sizeof msg)
    {
        perror ("server read");
        fprintf (stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
        goto out;
    }

    rem_dest = malloc (sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    // Parse and store the address information of the client
    sscanf (msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid (gid, &rem_dest->gid);

    // Connect to the Queue Pair of the client.
    if (pp_connect_ctx (ctx, ib_port, my_dest->psn, mtu, sl, rem_dest, sgid_idx))
    {
        fprintf (stderr, "Couldn't connect to remote QP\n");
        free (rem_dest);
        rem_dest = NULL;
        goto out;
    }

    // Create the message to send to the client, containing the local address.
    gid_to_wire_gid (&my_dest->gid, gid);
    sprintf (msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);

    // Send the message to the client.
    if (write (connfd, msg, sizeof msg) != sizeof msg || read (connfd, msg, sizeof msg) != sizeof "done")
    {
        fprintf (stderr, "Couldn't send/recv local address\n");
        free (rem_dest);
        rem_dest = NULL;
        goto out;
    }

out:
    close (connfd);
    return rem_dest;
}

static struct pingpong_context *
pp_init_ctx (struct ibv_device *ib_dev, int size, int rx_depth, int port, int use_event, int iters)
{
    struct pingpong_context *ctx;
    int access_flags = IBV_ACCESS_LOCAL_WRITE;

    ctx = calloc (1, sizeof *ctx);
    if (!ctx)
        return NULL;

    ctx->size = size;
    ctx->send_flags = IBV_SEND_SIGNALED;
    ctx->rx_depth = rx_depth;

    ctx->buf = memalign (page_size, size);
    if (!ctx->buf)
    {
        fprintf (stderr, "Couldn't allocate work buf.\n");
        goto clean_ctx;
    }

    memset (ctx->buf, 0x0, size);

    ctx->context = ibv_open_device (ib_dev);
    if (!ctx->context)
    {
        fprintf (stderr, "Couldn't get context for %s\n", ibv_get_device_name (ib_dev));
        goto clean_buffer;
    }

    if (use_event)
    {
        ctx->channel = ibv_create_comp_channel (ctx->context);
        if (!ctx->channel)
        {
            fprintf (stderr, "Couldn't create completion channel\n");
            goto clean_device;
        }
    }
    else
        ctx->channel = NULL;

    ctx->pd = ibv_alloc_pd (ctx->context);
    if (!ctx->pd)
    {
        fprintf (stderr, "Couldn't allocate PD\n");
        goto clean_comp_channel;
    }

    if (use_odp || use_ts || use_dm)
    {
        const uint32_t rc_caps_mask = IBV_ODP_SUPPORT_SEND | IBV_ODP_SUPPORT_RECV;
        struct ibv_device_attr_ex attrx;

        if (ibv_query_device_ex (ctx->context, NULL, &attrx))
        {
            fprintf (stderr, "Couldn't query device for its features\n");
            goto clean_pd;
        }

        if (use_odp)
        {
            if (!(attrx.odp_caps.general_caps & IBV_ODP_SUPPORT) || (attrx.odp_caps.per_transport_caps.rc_odp_caps & rc_caps_mask) != rc_caps_mask)
            {
                fprintf (stderr, "The device isn't ODP capable or does not support RC send and receive with ODP\n");
                goto clean_pd;
            }
            if (implicit_odp && !(attrx.odp_caps.general_caps & IBV_ODP_SUPPORT_IMPLICIT))
            {
                fprintf (stderr, "The device doesn't support implicit ODP\n");
                goto clean_pd;
            }
            access_flags |= IBV_ACCESS_ON_DEMAND;
        }

        if (use_ts)
        {
            if (!attrx.completion_timestamp_mask)
            {
                fprintf (stderr, "The device isn't completion timestamp capable\n");
                goto clean_pd;
            }
            ctx->completion_timestamp_mask = attrx.completion_timestamp_mask;
        }

        if (use_dm)
        {
            struct ibv_alloc_dm_attr dm_attr = {};

            if (!attrx.max_dm_size)
            {
                fprintf (stderr, "Device doesn't support dm allocation\n");
                goto clean_pd;
            }

            if (attrx.max_dm_size < size)
            {
                fprintf (stderr, "Device memory is insufficient\n");
                goto clean_pd;
            }

            dm_attr.length = size;
            ctx->dm = ibv_alloc_dm (ctx->context, &dm_attr);
            if (!ctx->dm)
            {
                fprintf (stderr, "Dev mem allocation failed\n");
                goto clean_pd;
            }

            access_flags |= IBV_ACCESS_ZERO_BASED;
        }
    }

    if (implicit_odp)
    {
        ctx->mr = ibv_reg_mr (ctx->pd, NULL, SIZE_MAX, access_flags);
    }
    else
    {
        ctx->mr = use_dm ? ibv_reg_dm_mr (ctx->pd, ctx->dm, 0, size, access_flags) : ibv_reg_mr (ctx->pd, ctx->buf, size, access_flags);
    }

    if (!ctx->mr)
    {
        fprintf (stderr, "Couldn't register MR\n");
        goto clean_dm;
    }

    if (prefetch_mr)
    {
        struct ibv_sge sg_list;
        int ret;

        sg_list.lkey = ctx->mr->lkey;
        sg_list.addr = (uintptr_t) ctx->buf;
        sg_list.length = size;

        ret = ibv_advise_mr (ctx->pd, IBV_ADVISE_MR_ADVICE_PREFETCH_WRITE, IB_UVERBS_ADVISE_MR_FLAG_FLUSH, &sg_list, 1);

        if (ret)
            fprintf (stderr, "Couldn't prefetch MR(%d). Continue anyway\n", ret);
    }

    if (use_ts)
    {
        struct ibv_cq_init_attr_ex attr_ex = {.cqe = rx_depth + 1, .cq_context = NULL, .channel = ctx->channel, .comp_vector = 0, .wc_flags = IBV_WC_EX_WITH_COMPLETION_TIMESTAMP};

        ctx->cq_s.cq_ex = ibv_create_cq_ex (ctx->context, &attr_ex);
    }
    else
    {
        ctx->cq_s.cq = ibv_create_cq (ctx->context, rx_depth + 1, NULL, ctx->channel, 0);
    }

    if (!pp_cq (ctx))
    {
        fprintf (stderr, "Couldn't create CQ\n");
        goto clean_mr;
    }

    {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr = {.send_cq = pp_cq (ctx), .recv_cq = pp_cq (ctx), .cap = {.max_send_wr = 1, .max_recv_wr = rx_depth, .max_send_sge = 1, .max_recv_sge = 1}, .qp_type = IBV_QPT_RC, .sq_sig_all = 0};

        if (use_new_send)
        {
            struct ibv_qp_init_attr_ex init_attr_ex = {};

            init_attr_ex.send_cq = pp_cq (ctx);
            init_attr_ex.recv_cq = pp_cq (ctx);
            init_attr_ex.cap.max_send_wr = 1;
            init_attr_ex.cap.max_recv_wr = rx_depth;
            init_attr_ex.cap.max_send_sge = 1;
            init_attr_ex.cap.max_recv_sge = 1;
            init_attr_ex.qp_type = IBV_QPT_RC;

            init_attr_ex.comp_mask |= IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
            init_attr_ex.pd = ctx->pd;
            init_attr_ex.send_ops_flags = IBV_QP_EX_WITH_SEND;

            ctx->qp = ibv_create_qp_ex (ctx->context, &init_attr_ex);
        }
        else
        {
            ctx->qp = ibv_create_qp (ctx->pd, &init_attr);
        }

        if (!ctx->qp)
        {
            fprintf (stderr, "Couldn't create QP\n");
            goto clean_cq;
        }

        if (use_new_send)
            ctx->qpx = ibv_qp_to_qp_ex (ctx->qp);

        ibv_query_qp (ctx->qp, &attr, IBV_QP_CAP, &init_attr);
        if (init_attr.cap.max_inline_data >= size && !use_dm)
            ctx->send_flags |= IBV_SEND_INLINE;
    }

    {
        struct ibv_qp_attr attr = {.qp_state = IBV_QPS_INIT, .pkey_index = 0, .port_num = port, .qp_access_flags = 0};

        if (ibv_modify_qp (ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        {
            fprintf (stderr, "Failed to modify QP to INIT\n");
            goto clean_qp;
        }
    }

    return ctx;

clean_qp:
    ibv_destroy_qp (ctx->qp);

clean_cq:
    ibv_destroy_cq (pp_cq (ctx));

clean_mr:
    ibv_dereg_mr (ctx->mr);

clean_dm:
    if (ctx->dm)
        ibv_free_dm (ctx->dm);

clean_pd:
    ibv_dealloc_pd (ctx->pd);

clean_comp_channel:
    if (ctx->channel)
        ibv_destroy_comp_channel (ctx->channel);

clean_device:
    ibv_close_device (ctx->context);

clean_buffer:
    free (ctx->buf);

clean_ctx:
    free (ctx);

    return NULL;
}

static int
pp_close_ctx (struct pingpong_context *ctx)
{
    if (ibv_destroy_qp (ctx->qp))
    {
        fprintf (stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq (pp_cq (ctx)))
    {
        fprintf (stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr (ctx->mr))
    {
        fprintf (stderr, "Couldn't deregister MR\n");
        return 1;
    }

    if (ctx->dm)
    {
        if (ibv_free_dm (ctx->dm))
        {
            fprintf (stderr, "Couldn't free DM\n");
            return 1;
        }
    }

    if (ibv_dealloc_pd (ctx->pd))
    {
        fprintf (stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ctx->channel)
    {
        if (ibv_destroy_comp_channel (ctx->channel))
        {
            fprintf (stderr, "Couldn't destroy completion channel\n");
            return 1;
        }
    }

    if (ibv_close_device (ctx->context))
    {
        fprintf (stderr, "Couldn't release context\n");
        return 1;
    }

    free (ctx->buf);
    free (ctx);

    return 0;
}

static int
pp_post_recv (struct pingpong_context *ctx, int n)
{
    struct ibv_sge list = {.addr = use_dm ? 0 : (uintptr_t) ctx->buf, .length = ctx->size, .lkey = ctx->mr->lkey};
    struct ibv_recv_wr wr = {
        .wr_id = PINGPONG_RECV_WRID,
        .sg_list = &list,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < n; ++i)
        if (ibv_post_recv (ctx->qp, &wr, &bad_wr))
            break;

    //LOG ("Posted %d receives\n", i);

    return i;
}

static int
pp_post_send (struct pingpong_context *ctx)
{
    struct ibv_sge list = {.addr = use_dm ? 0 : (uintptr_t) ctx->buf, .length = ctx->size, .lkey = ctx->mr->lkey};
    struct ibv_send_wr wr = {
        .wr_id = PINGPONG_SEND_WRID,
        .sg_list = &list,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = ctx->send_flags,
    };
    struct ibv_send_wr *bad_wr;

    //LOG ("Posting send\n");

    if (use_new_send)
    {
        ibv_wr_start (ctx->qpx);

        ctx->qpx->wr_id = PINGPONG_SEND_WRID;
        ctx->qpx->wr_flags = ctx->send_flags;

        ibv_wr_send (ctx->qpx);
        ibv_wr_set_sge (ctx->qpx, list.lkey, list.addr, list.length);

        return ibv_wr_complete (ctx->qpx);
    }
    else
    {
        return ibv_post_send (ctx->qp, &wr, &bad_wr);
    }
}

struct ts_params {
    uint64_t comp_recv_max_time_delta;
    uint64_t comp_recv_min_time_delta;
    uint64_t comp_recv_total_time_delta;
    uint64_t comp_recv_prev_time;
    int last_comp_with_ts;
    unsigned int comp_with_time_iters;
};

static inline int
parse_single_wc (struct pingpong_context *ctx, int *scnt, int *rcnt, int *routs, int iters, uint64_t wr_id,
                 enum ibv_wc_status status, uint64_t completion_timestamp, struct ts_params *ts, bool is_server)
{
    if (status != IBV_WC_SUCCESS)
    {
        fprintf (stderr, "Failed status %s (%d) for wr_id %d\n", ibv_wc_status_str (status), status, (int) wr_id);
        return 1;
    }

    switch ((int) wr_id)
    {
    case PINGPONG_SEND_WRID:
        //LOG ("Send event completed\n");
        ++(*scnt);
        break;

    case PINGPONG_RECV_WRID:
        //LOG ("Recv event completed\n");

        if (is_server)
        {
            //LOG ("Step 2: Received packet from client");
            update_payload (ctx->pp_buf, 2);
        }
        else
        {
            //LOG ("Step 4: Received packet from server");
            update_payload (ctx->pp_buf, 4);
            store_payload (ctx->pp_buf, pp_data);
        }

        if (--(*routs) <= 1)
        {
            *routs += pp_post_recv (ctx, ctx->rx_depth - *routs);
            if (*routs < ctx->rx_depth)
            {
                fprintf (stderr, "Couldn't post receive (%d)\n", *routs);
                return 1;
            }
        }

        ++(*rcnt);
        if (use_ts)
        {
            if (ts->last_comp_with_ts)
            {
                uint64_t delta;

                /* checking whether the clock was wrapped around */
                if (completion_timestamp >= ts->comp_recv_prev_time)
                    delta = completion_timestamp - ts->comp_recv_prev_time;
                else
                    delta = ctx->completion_timestamp_mask - ts->comp_recv_prev_time + completion_timestamp + 1;

                ts->comp_recv_max_time_delta = max (ts->comp_recv_max_time_delta, delta);
                ts->comp_recv_min_time_delta = min (ts->comp_recv_min_time_delta, delta);
                ts->comp_recv_total_time_delta += delta;
                ts->comp_with_time_iters++;
            }

            ts->comp_recv_prev_time = completion_timestamp;
            ts->last_comp_with_ts = 1;
        }
        else
        {
            ts->last_comp_with_ts = 0;
        }

        break;

    default:
        fprintf (stderr, "Completion for unknown wr_id %d\n", (int) wr_id);
        return 1;
    }

    ctx->pending &= ~(int) wr_id;
    if (*scnt < iters && !ctx->pending)
    {
        if (is_server)
        {
            //LOG ("Step 3: Sending packet back");
            update_payload (ctx->pp_buf, 3);
        }
        else
        {
            //LOG ("Step 1: Sending packet to server");
            update_payload (ctx->pp_buf, 1);
        }
        if (pp_post_send (ctx))
        {
            fprintf (stderr, "Couldn't post send\n");
            return 1;
        }
        ctx->pending = PINGPONG_RECV_WRID | PINGPONG_SEND_WRID;
    }

    return 0;
}

static void
usage (const char *argv0)
{
    printf ("Usage:\n");
    printf ("  %s            start a server and wait for connection\n", argv0);
    printf ("  %s <host>     connect to server at <host>\n", argv0);
    printf ("\n");
    printf ("Options:\n");
    printf ("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
    printf ("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
    printf ("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
    printf ("  -s, --size=<size>      size of message to exchange (default 4096)\n");
    printf ("  -m, --mtu=<size>       path MTU (default 1024)\n");
    printf ("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
    printf ("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
    printf ("  -l, --sl=<sl>          service level value\n");
    printf ("  -e, --events           sleep on CQ events (default poll)\n");
    printf ("  -g, --gid-idx=<gid index> local port gid index\n");
    printf ("  -o, --odp		    use on demand paging\n");
    printf ("  -O, --iodp		    use implicit on demand paging\n");
    printf ("  -P, --prefetch	    prefetch an ODP MR\n");
    printf ("  -t, --ts	            get CQE with timestamp\n");
    printf ("  -c, --chk	            validate received buffer\n");
    printf ("  -j, --dm	            use device memory\n");
    printf ("  -N, --new_send            use new post send WR API\n");
}

int main (int argc, char *argv[])
{
    // List of devices available in the local system
    struct ibv_device **dev_list;
    // Device to be used by the application
    struct ibv_device *ib_dev;
    // Context of the pingpong app
    struct pingpong_context *ctx;
    // "Address" of the local machine
    struct pingpong_dest my_dest;
    // "Address" of the remote machine
    struct pingpong_dest *rem_dest;
    // Start and end timestamps of the pingpong
    struct timeval start, end;
    // Name of the network device to be used (optional: the first one is used if unspecified)
    char *ib_devname = NULL;
    // Name of the server to connect to (client-mode only)
    char *servername = NULL;
    // Port of the communication
    unsigned int port = 18515;
    // InfiniBand port to be used in the device
    int ib_port = 1;
    // Size of the message to be exchanged
    unsigned int size = 1024;
    // MTU of the communication
    enum ibv_mtu mtu = IBV_MTU_1024;
    // Number of receives to post at a time
    unsigned int rx_depth = 500;
    // Number of iterations to perform (i.e. number of messages to exchange)
    unsigned int iters = 1000;
    // Sleep on CQ events (default poll)
    int use_event = 0;

    // Number of "receive" actually posted
    int routs;
    // Number of received and sent messages
    int rcnt, scnt;
    // Number of CQ events
    int num_cq_events = 0;
    // Service level value, used to prioritize packets. The higher the value, the higher the priority.
    int sl = 0;
    // Index of the GID to be used.
    int gidx = -1;
    // GID of the local machine
    char gid[33];
    // Data about the timestamp of the messages.
    struct ts_params ts;

    // Randomize the random generator seed.
    // This is useful, since the initial PSN is random.
    srand48 (getpid () * time (NULL));

    while (1)
    {
        int c;

        static struct option long_options[] = {{.name = "port", .has_arg = 1, .val = 'p'},
                                               {.name = "ib-dev", .has_arg = 1, .val = 'd'},
                                               {.name = "ib-port", .has_arg = 1, .val = 'i'},
                                               {.name = "size", .has_arg = 1, .val = 's'},
                                               {.name = "mtu", .has_arg = 1, .val = 'm'},
                                               {.name = "rx-depth", .has_arg = 1, .val = 'r'},
                                               {.name = "iters", .has_arg = 1, .val = 'n'},
                                               {.name = "sl", .has_arg = 1, .val = 'l'},
                                               {.name = "events", .has_arg = 0, .val = 'e'},
                                               {.name = "gid-idx", .has_arg = 1, .val = 'g'},
                                               {.name = "odp", .has_arg = 0, .val = 'o'},
                                               {.name = "iodp", .has_arg = 0, .val = 'O'},
                                               {.name = "prefetch", .has_arg = 0, .val = 'P'},
                                               {.name = "ts", .has_arg = 0, .val = 't'},
                                               {.name = "chk", .has_arg = 0, .val = 'c'},
                                               {.name = "dm", .has_arg = 0, .val = 'j'},
                                               {.name = "new_send", .has_arg = 0, .val = 'N'},
                                               {}};

        // Retrieve the next option name
        c = getopt_long (argc, argv, "p:d:i:s:m:r:n:l:eg:oOPtcjN", long_options, NULL);

        // If there is no more option to parse, stop
        if (c == -1)
            break;

        switch (c)
        {
        case 'p':
            port = strtoul (optarg, NULL, 0);
            if (port > 65535)
            {
                usage (argv[0]);
                return 1;
            }
            break;

        case 'd':
            ib_devname = strdupa (optarg);
            break;

        case 'i':
            ib_port = strtol (optarg, NULL, 0);
            if (ib_port < 1)
            {
                usage (argv[0]);
                return 1;
            }
            break;

        case 's':
            size = strtoul (optarg, NULL, 0);
            break;

        case 'm':
            mtu = pp_mtu_to_enum (strtol (optarg, NULL, 0));
            if (mtu == 0)
            {
                usage (argv[0]);
                return 1;
            }
            break;

        case 'r':
            rx_depth = strtoul (optarg, NULL, 0);
            break;

        case 'n':
            iters = strtoul (optarg, NULL, 0);
            break;

        case 'l':
            sl = strtol (optarg, NULL, 0);
            break;

        case 'e':
            ++use_event;
            break;

        case 'g':
            gidx = strtol (optarg, NULL, 0);
            break;

        case 'o':
            use_odp = 1;
            break;
        case 'P':
            prefetch_mr = 1;
            break;
        case 'O':
            use_odp = 1;
            implicit_odp = 1;
            break;
        case 't':
            use_ts = 1;
            break;
        case 'c':
            validate_buf = 1;
            break;

        case 'j':
            use_dm = 1;
            break;

        case 'N':
            use_new_send = 1;
            break;

        default:
            usage (argv[0]);
            return 1;
        }
    }

    if (optind == argc - 1)
        // There is a non-optional argument, i.e. the IP of the server to connect to
        // Therefore, we are in client mode
        servername = strdupa (argv[optind]);
    else if (optind < argc)
    {
        // There are more than one non-optional arguments, which is an error
        usage (argv[0]);
        return 1;
    }

    // Device Memory and On-Demand Paging are mutually exclusive, since Device Memory is handled without the
    // use of CPU; therefore, it is not possible to handle a page fault and on-demand paging cannot be used.
    if (use_odp && use_dm)
    {
        fprintf (stderr, "DM memory region can't be on demand\n");
        return 1;
    }

    // Prefetching make sense only if on-demand paging is used, otherwise the pages are already in memory
    if (!use_odp && prefetch_mr)
    {
        fprintf (stderr, "prefetch is valid only with on-demand memory region\n");
        return 1;
    }

    // Initialize pingpong payload data
    pp_data = init_pingpong_data (iters);

    if (use_ts)
    {
        ts.comp_recv_max_time_delta = 0;
        ts.comp_recv_min_time_delta = 0xffffffff;
        ts.comp_recv_total_time_delta = 0;
        ts.comp_recv_prev_time = 0;
        ts.last_comp_with_ts = 0;
        ts.comp_with_time_iters = 0;
    }

    page_size = sysconf (_SC_PAGESIZE);

    // Retrieve the list of IB devices available in the local system
    dev_list = ibv_get_device_list (NULL);
    if (!dev_list)
    {
        perror ("Failed to get IB devices list");
        return 1;
    }

    // If no device name is specified, use the first one in the list
    if (!ib_devname)
    {
        ib_dev = *dev_list;
        if (!ib_dev)
        {
            fprintf (stderr, "No IB devices found\n");
            return 1;
        }
    }
    else
    {
        int i;
        for (i = 0; dev_list[i]; ++i)
            if (!strcmp (ibv_get_device_name (dev_list[i]), ib_devname))
                break;
        ib_dev = dev_list[i];
        if (!ib_dev)
        {
            fprintf (stderr, "IB device %s not found\n", ib_devname);
            return 1;
        }
    }

    // Create the context of the pingpong app
    ctx = pp_init_ctx (ib_dev, size, rx_depth, ib_port, use_event, iters);
    if (!ctx)
        return 1;

    // Post the receive buffers.
    // `pp_post_recv` returns the actual number of posted `receive`.
    routs = pp_post_recv (ctx, ctx->rx_depth);
    if (routs < ctx->rx_depth)
    {
        fprintf (stderr, "Couldn't post receive (%d)\n", routs);
        return 1;
    }

    // Completion notifications must be enabled if needed
    if (use_event)
        // Request notifications from the cq of the app
        if (ibv_req_notify_cq (pp_cq (ctx), 0))
        {
            fprintf (stderr, "Couldn't request CQ notification\n");
            return 1;
        }

    // Retrieve information about the port used in the device...
    if (pp_get_port_info (ctx->context, ib_port, &ctx->portinfo))
    {
        fprintf (stderr, "Couldn't get port info\n");
        return 1;
    }

    // ... and set it in the "local address"
    my_dest.lid = ctx->portinfo.lid;
    // If RoCE is not used, the LID *must* be specified, since it identifies the destination; in RoCE, Ethernet is used.
    if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET && !my_dest.lid)
    {
        fprintf (stderr, "Couldn't get local LID\n");
        return 1;
    }

    if (gidx >= 0)
    {
        // If a GID index is specified, retrieve the GID of the local machine
        if (ibv_query_gid (ctx->context, ib_port, gidx, &my_dest.gid))
        {
            fprintf (stderr, "can't read sgid of index %d\n", gidx);
            return 1;
        }
    }
    else
        // Otherwise, use the default GID
        memset (&my_dest.gid, 0, sizeof my_dest.gid);

    // Retrieve the QP number of the context
    my_dest.qpn = ctx->qp->qp_num;

    // Generate a random PSN
    my_dest.psn = lrand48 () & 0xffffff;

    // Retrieve the GID as a IPv6 address
    inet_ntop (AF_INET6, &my_dest.gid, gid, sizeof gid);
    printf ("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n", my_dest.lid, my_dest.qpn, my_dest.psn,
            gid);

    // Find information about the remote machine
    if (servername)
        rem_dest = pp_client_exch_dest (servername, port, &my_dest);
    else
        rem_dest = pp_server_exch_dest (ctx, ib_port, mtu, port, sl, &my_dest, gidx);

    if (!rem_dest)
        return 1;

    inet_ntop (AF_INET6, &rem_dest->gid, gid, sizeof gid);
    printf ("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n", rem_dest->lid, rem_dest->qpn,
            rem_dest->psn, gid);

    if (servername)
        if (pp_connect_ctx (ctx, ib_port, my_dest.psn, mtu, sl, rem_dest, gidx))
            return 1;

    ctx->pending = PINGPONG_RECV_WRID;

    if (servername)
    {
        if (validate_buf)
            for (int i = 0; i < size; i += page_size)
                ctx->buf[i] = i / page_size % sizeof (char);

        if (use_dm)
            if (ibv_memcpy_to_dm (ctx->dm, 0, (void *) ctx->buf, size))
            {
                fprintf (stderr, "Copy to dm buffer failed\n");
                return 1;
            }

        update_payload (ctx->pp_buf, 1);
        if (pp_post_send (ctx))
        {
            fprintf (stderr, "Couldn't post send\n");
            return 1;
        }
        ctx->pending |= PINGPONG_SEND_WRID;
    }

    if (gettimeofday (&start, NULL))
    {
        perror ("gettimeofday");
        return 1;
    }

    rcnt = scnt = 0;
    while (rcnt < iters || scnt < iters)
    {
        //LOG ("Receive count: %d, Send count: %d\n", rcnt, scnt);
        int ret;

        if (use_event)
        {
            struct ibv_cq *ev_cq;
            void *ev_ctx;

            if (ibv_get_cq_event (ctx->channel, &ev_cq, &ev_ctx))
            {
                fprintf (stderr, "Failed to get cq_event\n");
                return 1;
            }

            ++num_cq_events;

            if (ev_cq != pp_cq (ctx))
            {
                fprintf (stderr, "CQ event for unknown CQ %p\n", ev_cq);
                return 1;
            }

            if (ibv_req_notify_cq (pp_cq (ctx), 0))
            {
                fprintf (stderr, "Couldn't request CQ notification\n");
                return 1;
            }
        }

        if (use_ts)
        {
            struct ibv_poll_cq_attr attr = {};

            do
            {
                ret = ibv_start_poll (ctx->cq_s.cq_ex, &attr);
            } while (!use_event && ret == ENOENT);

            if (ret)
            {
                fprintf (stderr, "poll CQ failed %d\n", ret);
                return ret;
            }
            ret = parse_single_wc (ctx, &scnt, &rcnt, &routs, iters, ctx->cq_s.cq_ex->wr_id, ctx->cq_s.cq_ex->status,
                                   ibv_wc_read_completion_ts (ctx->cq_s.cq_ex), &ts, servername == NULL);
            if (ret)
            {
                ibv_end_poll (ctx->cq_s.cq_ex);
                return ret;
            }
            ret = ibv_next_poll (ctx->cq_s.cq_ex);
            if (!ret)
                ret = parse_single_wc (ctx, &scnt, &rcnt, &routs, iters, ctx->cq_s.cq_ex->wr_id, ctx->cq_s.cq_ex->status,
                                       ibv_wc_read_completion_ts (ctx->cq_s.cq_ex), &ts, servername == NULL);
            ibv_end_poll (ctx->cq_s.cq_ex);
            if (ret && ret != ENOENT)
            {
                fprintf (stderr, "poll CQ failed %d\n", ret);
                return ret;
            }
        }
        else
        {
            int ne, i;
            struct ibv_wc wc;

            do
            {
                ne = ibv_poll_cq (pp_cq (ctx), 1, &wc);
                //LOG ("Polled %d CQ events\n", ne);
                if (ne < 0)
                {
                    fprintf (stderr, "poll CQ failed %d\n", ne);
                    return 1;
                }
            } while (!use_event && ne < 1);

            for (i = 0; i < ne; ++i)
            {
                ret = parse_single_wc (ctx, &scnt, &rcnt, &routs, iters, wc.wr_id, wc.status, 0, &ts, servername == NULL);
                if (ret)
                {
                    fprintf (stderr, "parse WC failed %d\n", ne);
                    return 1;
                }
            }
        }
    }

    if (gettimeofday (&end, NULL))
    {
        perror ("gettimeofday");
        return 1;
    }

    {
        float usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        long long bytes = (long long) size * iters * 2;

        printf ("%lld bytes in %.2f seconds = %.2f Mbit/sec\n", bytes, usec / 1000000., bytes * 8. / usec);
        printf ("%d iters in %.2f seconds = %.2f usec/iter\n", iters, usec / 1000000., usec / iters);

        if (use_ts && ts.comp_with_time_iters)
        {
            printf ("Max receive completion clock cycles = %" PRIu64 "\n", ts.comp_recv_max_time_delta);
            printf ("Min receive completion clock cycles = %" PRIu64 "\n", ts.comp_recv_min_time_delta);
            printf ("Average receive completion clock cycles = %f\n",
                    (double) ts.comp_recv_total_time_delta / ts.comp_with_time_iters);
        }

        if ((!servername) && (validate_buf))
        {
            if (use_dm)
                if (ibv_memcpy_from_dm (ctx->buf, ctx->dm, 0, size))
                {
                    fprintf (stderr, "Copy from DM buffer failed\n");
                    return 1;
                }
            for (int i = 0; i < size; i += page_size)
                if (ctx->buf[i] != i / page_size % sizeof (char))
                    printf ("invalid data in page %d\n", i / page_size);
        }
    }

    if (servername)
        save_payloads_to_file (pp_data, 50, "results/rc/");

    ibv_ack_cq_events (pp_cq (ctx), num_cq_events);

    if (pp_close_ctx (ctx))
        return 1;

    free_pingpong_data (pp_data);

    ibv_free_device_list (dev_list);
    free (rem_dest);
    return 0;
}
