// Require information: Device name, Port GID Index, Server IP
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

#include "../../common/common.h"
#include "../../common/net.h"
#include "../../common/persistence.h"
#include "ccan/minmax.h"
#include "src/ib_net.h"
#include "src/pingpong.h"

#define IB_MTU (pp_mtu_to_enum (PACKET_SIZE))

#define RECEIVE_DEPTH 500
#define DEFAULT_PORT 18515
#define IB_PORT 1

// Priority (i.e. service level) for the traffic
#define PRIORITY 0

void usage (char *prog)
{
    fprintf (stderr, "Usage: %s <ib-device> <port-gid-idx> <action> [extra arguments]\n", prog);
    fprintf (stderr, "Actions:\n");
    fprintf (stderr, "\t- start: start the pingpong experiment\n");
    fprintf (stderr, "\t         on the server: %s <ib-device> <port-gid-idx> start <# of packets>\n", prog);
    fprintf (stderr, "\t         on the client: %s <ib-device> <port-gid-idx> start <# of packets> <packets interval (ns)> <server IP>\n", prog);
}

enum {
    PINGPONG_RECV_WRID = 1,// The receive work request ID
    PINGPONG_SEND_WRID = 2,// The send work request ID
};

static int page_size;

struct pingpong_context {
    int pending;// WID of the pending WR

    int send_flags;

    union {
        uint8_t *buf;
        struct pingpong_payload *payload;
    };

    struct ibv_context *context;

    struct ibv_pd *pd;
    uint64_t completion_timestamp_mask;

    struct ibv_mr *mr;
    struct ibv_cq_ex *cq;

    struct ibv_qp *qp;
    struct ibv_qp_ex *qpx;
};

static struct pingpong_context *pp_init_context (struct ibv_device *dev)
{
    struct pingpong_context *ctx = malloc (sizeof (*ctx));
    if (!ctx)
    {
        return NULL;
    }

    ctx->send_flags = IBV_SEND_SIGNALED;

    ctx->buf = memalign (page_size, size);
    if (!ctx->buf)
    {
        LOG (stdout, "Couldn't allocate work buf.\n");
        goto clean_ctx;
    }

    memset (ctx->buf, 0, size);

    ctx->context = ibv_open_device (dev);
    if (!ctx->context)
    {
        LOG (stdout, "Couldn't get context for %s.\n", ibv_get_device_name (dev));
        goto clean_buf;
    }

    ctx->pd = ibv_alloc_pd (ctx->context);
    if (!ctx->pd)
    {
        LOG (stdout, "Couldn't allocate PD.\n");
        goto clean_device;
    }

    // Using Timestamping
    {
        const uint32_t rc_caps_mask = IBV_ODP_SUPPORT_SEND | IBV_ODP_SUPPORT_RECV;
        struct ibv_device_attr_ex attrx;
        if (ibv_query_device_ex (ctx->context, NULL, &attrx))
        {
            LOG (stdout, "Couldn't get device attributes.\n");
            goto clean_pd;
        }

        if (!attrx.completion_timestamp_mask)
        {
            LOG (stdout, "Device doesn't support completion timestamping.\n");
            goto clean_pd;
        }
        ctx->completion_timestamp_mask = attrx.completion_timestamp_mask;
    }

    ctx->mr = ibv_reg_mr (ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr)
    {
        LOG (stdout, "Couldn't register MR.\n");
        goto clean_pd;
    }

    struct ibv_cq_init_attr_ex cq_attr_ex = {
        .cqe = RECEIVE_DEPTH + 1,
        .cq_context = NULL,
        .channel = NULL,
        .comp_vector = 0,
        .wc_flags = IBV_WC_EX_WITH_COMPLETION_TIMESTAMP};

    ctx->cq = ibv_create_cq_ex (ctx->context, &cq_attr_ex);

    if (!ctx->cq)
    {
        LOG (stdout, "Couldn't create CQ.\n");
        goto clean_mr;
    }

    // Using "new send" API
    {
        struct ibv_qp_init_attr_ex init_attr_ex = {
            .send_cq = (struct ibv_cq *) ctx->cq,
            .recv_cq = (struct ibv_cq *) ctx->cq,
            .cap = {
                .max_send_wr = 1,
                .max_recv_wr = RECEIVE_DEPTH,
                .max_send_sge = 1,
                .max_recv_sge = 1},
            .qp_type = IBV_QPT_RC,
            .comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS,
            .pd = ctx->pd,
            .send_ops_flags = IBV_QP_EX_WITH_SEND};

        ctx->qp = ibv_create_qp_ex (ctx->context, &init_attr_ex);

        if (!ctx->qp)
        {
            LOG (stdout, "Couldn't create QP.\n");
            goto clean_cq;
        }

        ctx->qpx = ibv_qp_to_qp_ex (ctx->qp);

        ctx->send_flags |= IBV_SEND_INLINE;// TODO: check whether this is possible
    }

    {
        struct ibv_qp_attr attr = {
            .qp_state = IBV_QPS_INIT,
            .pkey_index = 0,
            .port_num = IB_PORT,
            .qp_access_flags = 0};

        if (ibv_modify_qp (ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        {
            LOG (stdout, "Failed to modify QP to INIT.\n");
            goto clean_qp;
        }
    }

    return ctx;

clean_qp:
    ibv_destroy_qp (ctx->qp);
clean_cq:
    ibv_destroy_cq ((struct ibv_cq *) ctx->cq);
clean_mr:
    ibv_dereg_mr (ctx->mr);
clean_pd:
    ibv_dealloc_pd (ctx->pd);
clean_device:
    ibv_close_device (ctx->context);
clean_buf:
    free (ctx->buf);
clean_ctx:
    free (ctx);

    return NULL;
}

static int pp_ib_connect (struct pingpong_context *ctx, int port, int local_psn, enum ibv_mtu mtu, int sl, struct ib_node_info *dest, int gid_idx)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = mtu,
        .dest_qp_num = dest->qpn,
        .rq_psn = dest->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {.is_global = 0,
                    .dlid = dest->lid,
                    .sl = sl,
                    .src_path_bits = 0,
                    .port_num = port}};

    if (dest->gid.global.interface_id)
    {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh = (struct ibv_global_route){
            .hop_limit = 1,
            .dgid = dest->gid,
            .sgid_index = gid_idx};
    }

    if (ibv_modify_qp (ctx->qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
    {
        LOG (stdout, "Failed to modify QP to RTR.\n");
        return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = local_psn;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp (ctx->qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
    {
        LOG (stdout, "Failed to modify QP to RTS.\n");
        return 1;
    }

    return 0;
}

static int pp_post_send (struct pingpong_context *ctx)
{
    ibv_wr_start (ctx->qpx);
    ctx->qpx->wr_id = PINGPONG_SEND_WRID;
    ctx->qpx->wr_flags = ctx->send_flags;

    ibv_wr_send (ctx->qpx);
    ibv_wr_set_sge (ctx->qpx, ctx->mr->lkey, PACKET_SIZE, (uintptr_t) ctx->buf);
    return ibv_wr_complete (ctx->qpx);
}

int pp_send_single_packet (const char *buf __unused, const int packet_id, struct sockaddr_ll *dest_addr __unused, void *aux)
{
    struct pingpong_context *ctx = (struct pingpong_context *) aux;
    ctx->payload = new_pingpong_payload (packet_id);
    ctx->payload->ts[0] = get_time_ns ();
    return pp_post_send (ctx);
}

static int pp_post_recv (struct pingpong_context *ctx, int n)
{
    struct ibv_sge list = {
        .addr = (uintptr_t) ctx->buf,
        .length = PACKET_SIZE,
        .lkey = ctx->mr->lkey};

    struct ibv_recv_wr wr = {
        .wr_id = PINGPONG_RECV_WRID,
        .sg_list = &list,
        .num_sge = 1};

    struct ibv_recv_wr *bad_wr;

    int i;
    for (i = 0; i < n; i++)
        if (ibv_post_recv (ctx->qp, &wr, &bad_wr))
            break;

    return i;
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

    if (pp_get_port_info (ctx->context, IB_PORT, &ctx->portinfo))
    {
        fprintf (stderr, "Couldn't get port info\n");
        return 1;
    }

    struct ib_node_info local_info;
    if (ib_get_local_info (ctx->context, IB_PORT, port_gid_idx, ctx->qp, &local_info))
    {
        fprintf (stderr, "Couldn't get local info\n");
        return 1;
    }
    ib_print_node_info (&local_info);

    struct ib_node_info remote_info;
    if (exchange_data (server_ip, is_server, sizeof (local_info), (uint8_t *) &local_info, (uint8_t *) &remote_info))
    {
        fprintf (stderr, "Couldn't exchange data\n");
        return 1;
    }

    fprintf (stdout, "Exchange data successful\n");
    fflush (stdout);
    ib_print_node_info (&remote_info);

    pp_ib_connect (ctx, IB_PORT, local_info.psn, IB_MTU, PRIORITY, remote_info, port_gid_idx);

    ctx->pending = PINGPONG_RECV_WRID;

    if (!is_server)
    {
        // TODO change start_sending_packets to not modify the buffer, but just call the send_packet function passing
        // the next packet id. In XDP, the function will have to write in the buffer, in RDMA nothing will happen basically.
        start_sending_packets (iters, interval, (char *) ctx->buf, NULL, pp_send_single_packet, ctx);
    }
}