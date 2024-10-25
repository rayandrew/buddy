#include <stdio.h>
#include <unistd.h>
#include <cassert>
#include "rdma.h"
#include "util.h"
#include "sockets.h"

#define IB_PORT 1
#define GID_INDEX 0
#define COUNT 1000

// TODO: This needs be less than the link MTU. Currently on Sleipner it is 1500
// but we should consider increasing it to 5000.
//#define MTU IBV_MTU_4096
#define MTU IBV_MTU_1024

namespace buddy::rdma {

Context context;

void init()
{
  context.init();
}

Context::~Context()
{
    if (pd) {
        ibv_dealloc_pd(pd);
        pd = NULL;
    }

    if (ctx) {
        ibv_close_device(ctx);
        ctx = NULL;
    }
}

Context& Context::get()
{
  if (!context.get_ctx())
    context.init();

  return context;
}

void Context::init()
{
  struct ibv_device **dev_list;

  srand48(getpid() * time(NULL));

  // Get an IB devices list:
  dev_list = ibv_get_device_list(NULL);
  if (!dev_list)
  {
      perror("Failed to get IB devices list.");
      FAIL("ib setup failed");
  }

  // Get an IB device:
  if (!*dev_list)
  {
      perror("No IB devices found.");
      FAIL("ib setup failed");
  }

  /*
  // Select last device (dpu specifc hack)
  while (dev_list[1])
      dev_list++;
  */

  //printf("using ib device %s\n", ibv_get_device_name(*dev_list));

  // Open an IB device context:
  ctx = ibv_open_device(*dev_list);
  if (!ctx)
  {
    fprintf(stderr, "Couldn't get context for %s.\n", ibv_get_device_name(*dev_list));
    FAIL("ib setup failed");
  }

  ibv_free_device_list(dev_list);

  // Allocate a Protection Domain:
  pd = ibv_alloc_pd(ctx);
  if (!pd)
  {
    perror("Failed to allocate Protection Domain.");
    FAIL("ib setup failed");
  }

  // Query IB port attribute
  memset(&port_info, 0, sizeof(port_info));
  if(ibv_query_port(ctx, IB_PORT, &port_info))
  {
    perror("Failed to query IB port information.");
    FAIL("ib setup failed");
  }

  union ibv_gid gid;
  memset(&gid, 0, sizeof(gid));
  if (ibv_query_gid(ctx, IB_PORT, GID_INDEX, &gid)) {
    perror("Failed to query GID");
    FAIL("ib setup failed");
  }

  // Query Device attribute
  struct ibv_device_attr device_attr;
  if (ibv_query_device(ctx, &device_attr))
  {
      perror("Failed to query IB device information.");
      FAIL("ib setup failed");
  }
  /*else{
      printf("The maximum number of QP = %d\n", device_attr.max_qp);
      printf("Largest contiguous block that can be registered %llu\n", device_attr.max_mr_size);
      printf("Maximum number of outstanding WR = %d\n", device_attr.max_qp_wr);
  }*/

  local_dest_template = {};

  // Get LID:
  local_dest_template.lid = port_info.lid;
  if ( port_info.link_layer == IBV_LINK_LAYER_INFINIBAND && !local_dest_template.lid)
  {
      perror("Couldn't get LID.");
      FAIL("ib setup failed");
  }

  // Set PSN:
  local_dest_template.psn = lrand48() & 0xffffff;

  memcpy(&local_dest_template.gid.raw, &gid.raw, sizeof(gid.raw));
}

server_cqs::server_cqs()
{
  ibv_srq_init_attr srq_attr = {
    .attr = {
      .max_wr = COUNT,
      .max_sge = 1,
    },
  };

  srq = ibv_create_srq(context.get_pd(), &srq_attr);
  if (!srq) {
    perror("ibv_create_srq");
    FAIL("failed to create srq");
  }

  send = ibv_create_cq(context.get_ctx(), COUNT, NULL, NULL, 0);
  recv = ibv_create_cq(context.get_ctx(), COUNT, NULL, NULL, 0);
}

void server_cqs::destroy()
{
  CHECK(!ibv_destroy_cq(send));
  CHECK(!ibv_destroy_cq(recv));
  CHECK(!ibv_destroy_srq(srq));
}

QP::QP(int connfd, bool same_cq)
{
  send_cq = ibv_create_cq(context.get_ctx(), COUNT, NULL, NULL, 0);
  if (same_cq)
    recv_cq = send_cq;
  else
    recv_cq = ibv_create_cq(context.get_ctx(), COUNT, NULL, NULL, 0);
  own_cqs = true;

  if (!send_cq || !recv_cq) {
    perror("Couldn't create Completion Queue.");
    FAIL("qp setup failed");
  }

  setup_common(NULL, connfd);
}

QP::QP(server_cqs cqs, int connfd, bool inverse_server)
{
  recv_cq = cqs.recv;
  send_cq = cqs.send;
  own_cqs = false;

  setup_common(cqs.srq, connfd, inverse_server);
}

void QP::setup_common(ibv_srq *srq, int connfd, bool inverse_server)
{
  // Creates a Queue Pair:
  {
    struct ibv_qp_init_attr qp_init_attr = {
            .send_cq = send_cq,
            .recv_cq = recv_cq,
            .srq = srq,
            .cap = {
                    .max_send_wr = COUNT,
                    .max_recv_wr = COUNT,
                    .max_send_sge = 1,
                    .max_recv_sge = 1,
            },
            .qp_type = IBV_QPT_RC,
    };

    qp = ibv_create_qp(context.get_pd(), &qp_init_attr);
    if (!qp) {
        perror("Couldn't create Queue Pair.");
        FAIL("qp setup failed");
    }

    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state        = IBV_QPS_INIT;
    qp_attr.pkey_index      = 0;
    qp_attr.port_num        = IB_PORT;
    qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        perror("Failed to modify QP to INIT.");
        FAIL("ib setup failed");
    }
    
    ibv_query_qp(qp, &qp_attr, IBV_QP_CAP, &qp_init_attr);
    max_inline_data = qp_init_attr.cap.max_inline_data;
    //printf("The maximum inline data = %d\n", max_inline_data);
  }

  IBDest local_dest = *context.get_local_dest_template();
  IBDest remote_dest = {};

  // Get QPN:
  local_dest.qpn = qp->qp_num;

  // Exchange IBDest values
  if (srq && !inverse_server) {
    full_read(connfd, (char *)&remote_dest, sizeof(remote_dest));
    full_write(connfd, (char *)&local_dest, sizeof(local_dest));
  } else {
    full_write(connfd, (char *)&local_dest, sizeof(local_dest));
    full_read(connfd, (char *)&remote_dest, sizeof(remote_dest));
  }

  struct ibv_qp_attr qp_attr;
  memset(&qp_attr, 0, sizeof(qp_attr));
  qp_attr.qp_state		= IBV_QPS_RTR;
  qp_attr.path_mtu		= MTU;
  qp_attr.dest_qp_num	= remote_dest.qpn;
  qp_attr.rq_psn			= remote_dest.psn;
  qp_attr.max_dest_rd_atomic	= 1;
  qp_attr.min_rnr_timer		    = 12;

#if 0
  qp_attr.ah_attr.is_global	  = 0;
#else
  qp_attr.ah_attr.is_global	  = 1;
  memcpy(qp_attr.ah_attr.grh.dgid.raw, remote_dest.gid.raw, sizeof(remote_dest.gid.raw));
  qp_attr.ah_attr.grh.sgid_index = GID_INDEX;
  qp_attr.ah_attr.grh.hop_limit = 255;
  qp_attr.ah_attr.grh.traffic_class = 0;
#endif

  qp_attr.ah_attr.dlid  = remote_dest.lid;
  qp_attr.ah_attr.sl		= 0;
  qp_attr.ah_attr.src_path_bits	= 0;
  qp_attr.ah_attr.port_num	= IB_PORT;


  if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                  IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER | IBV_QP_AV)) {
      perror("Failed to modify QP to RTR.");
      FAIL("qp setup failed.");
  }

  qp_attr.qp_state	    = IBV_QPS_RTS;
  qp_attr.timeout	      = 14;
  qp_attr.retry_cnt	    = 7;
  // rnr_retry is if receiver does not have enough resources (recv buffers). 7=infinite.
  qp_attr.rnr_retry	    = 7;
  //qp_attr.rnr_retry	    = 0;
  qp_attr.sq_psn	      = local_dest.psn;
  qp_attr.max_rd_atomic = 1;
  if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                                  IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
  {
      perror("Failed to modify QP to RTS.\n");
      FAIL("qp setup failed.");
  }
}

QP::~QP()
{
  if (qp) {
    if (ibv_destroy_qp(qp)) {
      perror("ibv_destroy_qp");
      FAIL("ibv_destroy_qp");
    }
    qp = NULL;
  }

  if (own_cqs) {
    if (send_cq)
      ibv_destroy_cq(send_cq);

    if (recv_cq && recv_cq != send_cq) 
      ibv_destroy_cq(recv_cq);
  }

  send_cq = NULL;
  recv_cq = NULL;
}

void QP::send_imm(uint32_t tag, ibv_mr *mr, unsigned len, unsigned offset, uint64_t wr_id)
{
  if (len <= max_inline_data) {
    send_imm_inline(tag, mr->addr, len, offset, wr_id);
    return;
  }

  struct ibv_sge list = {
    .addr	  = (uint64_t) mr->addr + offset,
    .length = (uint32_t) len,
    .lkey	  = mr->lkey
  };

  struct ibv_send_wr *bad_wr;

  struct ibv_send_wr wr = {
    .wr_id = wr_id,
    .sg_list = &list,
    .num_sge = 1,
    .opcode = IBV_WR_SEND_WITH_IMM,
    .send_flags = IBV_SEND_SIGNALED,
    .imm_data = tag,
  };

  int err = ibv_post_send(qp, &wr, &bad_wr);
  if (err)
    FAIL("Failed to ibv_post_send: " << strerror(err));
}

void QP::send_imm_inline(uint32_t tag, void *buf, unsigned len, unsigned offset, uint64_t wr_id)
{
  if (len > max_inline_data) {
    FAIL("above max inline size " << len << " > " << max_inline_data);
  }

  struct ibv_sge list = {
    .addr	  = (uint64_t) buf + offset,
    .length = (uint32_t) len,
  };

  struct ibv_send_wr *bad_wr;

  struct ibv_send_wr wr = {
    .wr_id = wr_id,
    .sg_list = &list,
    .num_sge = !!len,
    .opcode = IBV_WR_SEND_WITH_IMM,
    .send_flags = IBV_SEND_INLINE | IBV_SEND_SIGNALED,
    .imm_data = tag,
  };

  int err = ibv_post_send(qp, &wr, &bad_wr);
  if (err == ENOMEM) {
    // Send queue is full (probably)
    FAIL("send queue full");
  } else if (err) {
    FAIL("Failed to ibv_post_send: " << strerror(err));
  }
}

void QP::wait_cq(ibv_wc *wc, ibv_cq *cq)
{
  int n;
  do {
    n = ibv_poll_cq(cq, 1, wc);
  } while (n == 0);
  CHECK(n > 0);

  if (wc->status != IBV_WC_SUCCESS) {
    std::cerr << "wc status " << wc->status << ": " << ibv_wc_status_str(wc->status) << std::endl;
    std::cerr << "vendor_err " << wc->vendor_err << std::endl;
    FAIL("wc error");
  }
}

void QP::wait_send(ibv_wc *wc)
{
  wait_cq(wc, send_cq);
  CHECK(!(wc->opcode & IBV_WC_RECV));
}

void QP::wait_recv(ibv_wc *wc)
{
  wait_cq(wc, recv_cq);
  CHECK(wc->opcode & IBV_WC_RECV);
}

bool QP::poll_cq(ibv_wc *wc, ibv_cq *cq)
{
  int n = ibv_poll_cq(cq, 1, wc);
  CHECK(n >= 0);

  if (n == 1) {
    if (wc->status != IBV_WC_SUCCESS) {
      FAIL("unsuccessful status " << wc->status << " (vendor_err " << wc->vendor_err << ")");
    }
    return true;
  } else {
    return false;
  }
}

bool QP::poll_recv(ibv_wc *wc)
{
  if (poll_cq(wc, recv_cq)) {
    CHECK(wc->opcode & IBV_WC_RECV);
    return true;
  } else {
    return false;
  }
}

void QP::recv(ibv_mr *mr, unsigned len, unsigned offset, uint64_t wr_id)
{
  ibv_sge list = {
    .addr = (uint64_t) mr->addr + offset,
    .length = (uint32_t) len,
    .lkey = mr->lkey,
  };

  ibv_recv_wr wr = {
    .wr_id = wr_id,
    .sg_list = &list,
    .num_sge = 1,
  };

  ibv_recv_wr *bad_wr;
  if (ibv_post_recv(qp, &wr, &bad_wr)) {
    FAIL("Failed to ibv_post_recv");
  }
}

static void prepare_write_imm(ibv_send_wr *wr, ibv_sge *sge, uint32_t tag, void *buf, ibv_mr *mr, unsigned len, uint64_t remote_addr, uint32_t rkey, uint64_t wr_id)
{
  ibv_send_wr w = {
    .wr_id = wr_id,
    .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
    .send_flags = IBV_SEND_SIGNALED,
    .imm_data = tag,
    .wr = {
      .rdma = {
        .remote_addr = remote_addr,
        .rkey = rkey,
      },
    },
  };

  if (len > 0) {
    assert(len <= mr->length);
    assert(buf >= mr->addr);
    assert(buf <= (char *)mr->addr + mr->length - len);

    *sge  = {
      .addr   = (uint64_t) buf,
      .length = (uint32_t) len,
      .lkey   = mr->lkey
    };

    w.sg_list = sge;
    w.num_sge = 1;
  }

  *wr = w;
}

void QP::write_imm(uint32_t tag, void *buf, ibv_mr *mr, unsigned len, uint64_t remote_addr, uint32_t rkey, uint64_t wr_id)
{
  struct ibv_send_wr wr;
  struct ibv_sge list;

  prepare_write_imm(&wr, &list, tag, buf, mr, len, remote_addr, rkey, wr_id);

  struct ibv_send_wr *bad_wr;
  if (ibv_post_send(qp, &wr, &bad_wr)) {
    FAIL("Failed to ibv_post_send");
  }
}

} // namespace buddy::rdma
