// Require information: Device name, Port GID Index, Server IP
#include <arpa/inet.h>
#include <getopt.h>
#include <inttypes.h>
#include <malloc.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../../common/common.h"
#include "../../common/net.h"
#include "../../common/persistence.h"
//#include "ccan/minmax.h"
#include "src/ib_net.h"
#include "src/pingpong.h"

#define IB_MTU (pp_mtu_to_enum (PACKET_SIZE))

#define RECEIVE_DEPTH 500
#define DEFAULT_PORT 18515
#define IB_PORT 1

// Priority (i.e. service level) for the traffic
#define PRIORITY 0

enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};

void usage (char *prog)
{
    fprintf (stderr, "Usage: %s <ib-device> <port-gid-idx> <action> [extra arguments]\n", prog);
    fprintf (stderr, "Actions:\n");
    fprintf (stderr, "\t- start: start the pingpong experiment\n");
    fprintf (stderr, "\t         on the server: %s <ib-device> <port-gid-idx> start <# of packets>\n", prog);
    fprintf (stderr, "\t         on the client: %s <ib-device> <port-gid-idx> start <# of packets> <packets interval (ns)> <server IP>\n", prog);
}

static int page_size;
static int available_recv = 0;

struct pingpong_context {
    atomic_uint_fast8_t pending;
    int send_flags;

    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_ah *ah;

    struct ib_node_info remote_info;

    union {
        uint8_t *send_buf;
        struct pingpong_payload *send_payload;
    };

    union {
        uint8_t *recv_buf;
        struct pingpong_payload *recv_payload;
    };
};

int init_pp_buffer (void **buffer, size_t size)
{
    *buffer = memalign (page_size, size + 40);
    if (!*buffer)
    {
        fprintf (stderr, "Couldn't allocate work buffer\n");
        return 1;
    }

    memset (*buffer, 0, size + 40);
    return 0;
}

struct pingpong_context *pp_init_context (struct ibv_device *ib_dev)
{
    struct pingpong_context *ctx = malloc (sizeof (struct pingpong_context));
    if (!ctx)
        return NULL;

    ctx->send_flags = IBV_SEND_SIGNALED;

    if (init_pp_buffer ((void **) &ctx->send_buf, PACKET_SIZE))
    {
        LOG (stderr, "Couldn't allocate send_buf\n");
        goto clean_ctx;
    }
    if (init_pp_buffer ((void **) &ctx->recv_buf, PACKET_SIZE))
    {
        LOG (stderr, "Couldn't allocate recv_buf\n");
        goto clean_send_buf;
    }

    ctx->context = ibv_open_device (ib_dev);
    if (!ctx->context)
    {
        LOG (stderr, "Couldn't get context for %s\n", ibv_get_device_name (ib_dev));
        goto clean_recv_buf;
    }

    ctx->pd = ibv_alloc_pd (ctx->context);
    if (!ctx->pd)
    {
        LOG (stderr, "Couldn't allocate PD\n");
        goto clean_context;
    }

    ctx->send_mr = ibv_reg_mr (ctx->pd, ctx->send_buf, PACKET_SIZE + 40, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->send_mr)
    {
        LOG (stderr, "Couldn't register MR for send_buf\n");
        goto clean_pd;
    }
    ctx->recv_mr = ibv_reg_mr (ctx->pd, ctx->recv_buf, PACKET_SIZE + 40, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->recv_mr)
    {
        LOG (stderr, "Couldn't register MR for recv_buf\n");
        goto clean_send_mr;
    }

    ctx->cq = ibv_create_cq (ctx->context, RECEIVE_DEPTH, NULL, NULL, 0);
    if (!ctx->cq)
    {
        LOG (stderr, "Couldn't create CQ\n");
        goto clean_recv_mr;
    }

    {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr = {
            .send_cq = ctx->cq,
            .recv_cq = ctx->cq,
            .cap = {
                .max_send_wr = 1,
                .max_recv_wr = RECEIVE_DEPTH,
                .max_send_sge = 1,
                .max_recv_sge = 1},
            .qp_type = IBV_QPT_UD,
        };

        ctx->qp = ibv_create_qp (ctx->pd, &init_attr);
        if (!ctx->qp)
        {
            LOG (stderr, "Couldn't create QP\n");
            goto clean_cq;
        }

        ibv_query_qp (ctx->qp, &attr, IBV_QP_CAP, &init_attr);
        if (init_attr.cap.max_inline_data >= PACKET_SIZE)
        {
            ctx->send_flags |= IBV_SEND_INLINE;
        }
        else
        {
            LOG (stdout, "Device doesn't support IBV_SEND_INLINE, using sge. Max inline size: %d\n", init_attr.cap.max_inline_data);
        }
    }

    {
        struct ibv_qp_attr attr = {
            .qp_state = IBV_QPS_INIT,
            .pkey_index = 0,
            .port_num = IB_PORT,
            .qkey = 0x11111111};
        if (ibv_modify_qp (ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY))
        {
            LOG (stderr, "Failed to modify QP to INIT\n");
            goto clean_qp;
        }
    }

    return ctx;

clean_qp:
    ibv_destroy_qp (ctx->qp);

clean_cq:
    ibv_destroy_cq (ctx->cq);

clean_recv_mr:
    ibv_dereg_mr (ctx->recv_mr);

clean_send_mr:
    ibv_dereg_mr (ctx->send_mr);

clean_pd:
    ibv_dealloc_pd (ctx->pd);

clean_context:
    ibv_close_device (ctx->context);

clean_recv_buf:
    free (ctx->recv_buf);

clean_send_buf:
    free (ctx->send_buf);

clean_ctx:
    free (ctx);
    return NULL;
}

int pp_close_context (struct pingpong_context *ctx)
{
    if (ibv_destroy_qp (ctx->qp))
    {
        LOG (stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq (ctx->cq))
    {
        LOG (stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr (ctx->recv_mr))
    {
        LOG (stderr, "Couldn't deregister MR for recv_buf\n");
        return 1;
    }

    if (ibv_dereg_mr (ctx->send_mr))
    {
        LOG (stderr, "Couldn't deregister MR for send_buf\n");
        return 1;
    }

    if (ibv_dealloc_pd (ctx->pd))
    {
        LOG (stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ibv_close_device (ctx->context))
    {
        LOG (stderr, "Couldn't close device context\n");
        return 1;
    }

    free (ctx->recv_buf);
    free (ctx->send_buf);
    free (ctx);
    return 0;
}

int pp_post_recv (struct pingpong_context *ctx, int n)
{
    struct ibv_sge list = {
        .addr = (uintptr_t) ctx->recv_buf,
        .length = PACKET_SIZE + 40,
        .lkey = ctx->recv_mr->lkey};
    struct ibv_recv_wr wr = {
        .wr_id = PINGPONG_RECV_WRID,
        .sg_list = &list,
        .num_sge = 1};
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < n; ++i)
        if (ibv_post_recv (ctx->qp, &wr, &bad_wr))
            break;

    LOG (stdout, "Posted %d receives\n", i);

    return i;
}
int pp_post_send (struct pingpong_context *ctx)
{
    const struct ib_node_info *remote = &ctx->remote_info;
    struct ibv_sge list = {
        .addr = (uintptr_t) ctx->send_buf + 40,
        .length = PACKET_SIZE,
        .lkey = ctx->send_mr->lkey};

    struct ibv_send_wr wr = {
        .wr_id = PINGPONG_SEND_WRID,
        .sg_list = &list,
        .num_sge = 1,
        .opcode = IBV_WR_SEND,
        .send_flags = ctx->send_flags,
        .wr = {
            .ud = {
                .ah = ctx->ah,
                .remote_qpn = remote->qpn,
                .remote_qkey = 0x11111111}}};
    struct ibv_send_wr *bad_wr;
    return ibv_post_send (ctx->qp, &wr, &bad_wr);
}

int parse_single_wc (struct pingpong_context *ctx, struct ibv_wc wc, const bool is_server)
{
    const uint64_t ts = get_time_ns ();
    if (wc.status != IBV_WC_SUCCESS)
    {
        LOG (stderr, "Failed status %s (%d) for wr_id %d\n", ibv_wc_status_str (wc.status), wc.status, (int) wc.wr_id);
        return 1;
    }

    switch ((int) wc.wr_id)
    {
    case PINGPONG_SEND_WRID:
        break;
    case PINGPONG_RECV_WRID:
        if (is_server)
        {
            memcpy (ctx->send_buf + 40, ctx->recv_buf + 40, PACKET_SIZE);
            ctx->send_payload->ts[1] = ts;
            ctx->send_payload->ts[2] = get_time_ns ();
            LOG (stdout, "Sending back packet %d\n", *(uint32_t*)(ctx->recv_buf));
            if (pp_post_send (ctx))
            {
                LOG (stderr, "Couldn't post send\n");
                return 1;
            }
        }
        else
        {
            ctx->recv_payload->ts[3] = get_time_ns ();
            //            printf ("Packet %d: %llu %llu %llu %llu\n", ctx->recv_payload->id, ctx->recv_payload->ts[0], ctx->recv_payload->ts[1], ctx->recv_payload->ts[2], ctx->recv_payload->ts[3]);
        }

        if (--available_recv <= 1)
        {
            available_recv += pp_post_recv (ctx, RECEIVE_DEPTH - available_recv);
            if (available_recv < RECEIVE_DEPTH)
            {
                LOG (stderr, "Couldn't post enough receives\n");
                return 1;
            }
        }
        break;
    default:
        LOG (stderr, "Completion for unknown wr_id %d\n", (int) wc.wr_id);
        return 1;
    }

    ctx->pending &= wc.wr_id;

    return 0;
}

int pp_ib_connect (struct pingpong_context *ctx, int gidx, struct ib_node_info *local, struct ib_node_info *dest)
{
    struct ibv_ah_attr ah_attr = {
        .is_global = 0,
        .dlid = dest->lid,
        .sl = PRIORITY,
        .src_path_bits = 0,
        .port_num = IB_PORT};
    struct ibv_qp_attr attr = {.qp_state = IBV_QPS_RTR};

    if (ibv_modify_qp (ctx->qp, &attr, IBV_QP_STATE))
    {
        LOG (stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = local->psn;

    if (ibv_modify_qp (ctx->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN))
    {
        LOG (stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    if (dest->gid.global.interface_id)
    {
        ah_attr.is_global = 1;
        ah_attr.grh.dgid = dest->gid;
        ah_attr.grh.sgid_index = gidx;
        ah_attr.grh.hop_limit = 1;
    }

    ctx->ah = ibv_create_ah (ctx->pd, &ah_attr);
    if (!ctx->ah)
    {
        LOG (stderr, "Failed to create AH\n");
        return 1;
    }

    return 0;
}

int pp_send_single_packet (char *buf __unused, const int packet_id, struct sockaddr_ll *dest_addr __unused, void *aux)
{
    struct pingpong_context *ctx = (struct pingpong_context *) aux;
    *ctx->send_payload = new_pingpong_payload (packet_id);
    ctx->send_payload->ts[0] = get_time_ns ();

    int ret = pp_post_send (ctx);
    if (ret)
    {
        LOG (stderr, "Couldn't post send\n");
    }
    return ret;
}

int main (int argc, char **argv)
{
    if (argc < 5)
    {
        usage (argv[0]);
        return 1;
    }

    char *ib_devname = argv[1];
    int port_gid_idx = strtol (argv[2], NULL, 0);
    char *action = argv[3];

    if (strncmp (action, "start", 5) != 0)
    {
        usage (argv[0]);
        return 1;
    }

    uint32_t iters = strtol (argv[4], NULL, 0);

    bool is_server = true;
    uint64_t interval = 0;
    char *server_ip = NULL;

    if (argc > 5)
    {
        is_server = false;
        interval = strtol (argv[5], NULL, 0);
        server_ip = argv[6];
    }

    LOG (stdout, "Server IP: %s\n", server_ip);

    srand48 (getpid () * time (NULL));

    page_size = sysconf (_SC_PAGESIZE);

    struct ibv_device *ib_dev = ib_device_find_by_name (ib_devname);
    if (!ib_dev)
    {
        fprintf (stderr, "IB device %s not found\n", ib_devname);
        return 1;
    }

    struct pingpong_context *ctx = pp_init_context (ib_dev);
    if (!ctx)
    {
        fprintf (stderr, "Couldn't initialize context\n");
        return 1;
    }

    struct ib_node_info local_info;
    if (ib_get_local_info (ctx->context, IB_PORT, port_gid_idx, ctx->qp, &local_info))
    {
        fprintf (stderr, "Couldn't get local info\n");
        pp_close_context (ctx);
        return 1;
    }
    ib_print_node_info (&local_info);

    if (exchange_data (server_ip, is_server, sizeof (local_info), (uint8_t *) &local_info, (uint8_t *) &ctx->remote_info))
    {
        fprintf (stderr, "Couldn't exchange data\n");
        pp_close_context (ctx);
        return 1;
    }
    ib_print_node_info (&ctx->remote_info);

    if (pp_ib_connect (ctx, port_gid_idx, &local_info, &ctx->remote_info))
    {
        fprintf (stderr, "Couldn't connect\n");
        pp_close_context (ctx);
        return 1;
    }

    LOG (stdout, "Connected\n");

    ctx->pending = PINGPONG_RECV_WRID;

    available_recv = pp_post_recv (ctx, RECEIVE_DEPTH);

    if (!is_server)
    {
        start_sending_packets (iters, interval, (char *) ctx->send_buf, NULL, pp_send_single_packet, ctx);
    }

    uint32_t recv_count = 0;
    while (recv_count < iters)
    {
        struct ibv_wc wc;
        int ne;

        do
        {
            ne = ibv_poll_cq (ctx->cq, 1, &wc);
            if (ne < 0)
            {
                fprintf (stderr, "Poll CQ failed %d\n", ne);
                return 1;
            }
        } while (!ne);

        if (parse_single_wc (ctx, wc, is_server))
        {
            fprintf (stderr, "Couldn't parse WC\n");
            return 1;
        }

        if (wc.wr_id == PINGPONG_RECV_WRID)
            recv_count++;
    }

    LOG (stdout, "Received all packets\n");

    if (pp_close_context (ctx))
    {
        fprintf (stderr, "Couldn't close context\n");
        return 1;
    }
    return 0;
}