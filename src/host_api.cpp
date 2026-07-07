#include <iostream>
#include <cassert>
#include "buddy.h"
#include "dpu_conn.h"
#include "util.h"
#include "rdma.h"
#include "local_proto.h"
#ifdef LOCAL_DMA
#include <queue>
#include <cstring>
#endif

using namespace buddy;

#ifdef LOCAL_DMA
// Local DMA: data rides the exported staging region; the RDMA QP carries control only.
// buddy_recv registers an app buffer; the proxy DMAs into RECV staging + sends IMM_D2H_DMA(size),
// and buddy_poll copies staging -> the (FIFO) app buffer. Correct for request-response patterns.
struct pending_recv { uint64_t id; char *dst; size_t len; };
static std::queue<pending_recv> g_pending_recvs;
static const int NCTRL = 8;
static char *g_ctrl_buf;      // NCTRL size-notification slots
static ibv_mr *g_ctrl_mr;
#endif

static int g_trace;
#define TRACE(l,x) do{if (g_trace>=(l)){std::clog << x << std::endl;}}while(0)

static size_t g_max_send;
static size_t g_min_recv;

static host::DpuConn *g_dpu_conn;

enum tt_clock {
  TT_SEND,
  TT_RECV,
  TT_POLL,
  TT_COUNT,
};

__attribute__((unused))
static const char *tt_label[TT_COUNT] = {
  "send",
  "recv",
  "poll",
};

#include "tictoc.h"

void buddy_init(MPI_Comm comm, size_t max_send, size_t min_recv)
{
  rdma::init();

  char *env = getenv("BUDDY_TRACE");
  if (env)
    g_trace = atoi(env);

  g_max_send = max_send;
  g_min_recv = min_recv;

  g_dpu_conn = new host::DpuConn(comm, max_send, min_recv);

#ifdef LOCAL_DMA
  g_ctrl_buf = new char[NCTRL * sizeof(uint64_t)];
  g_ctrl_mr = ibv_reg_mr(rdma::Context::get().get_pd(), g_ctrl_buf, NCTRL*sizeof(uint64_t),
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  CHECK(g_ctrl_mr);
  for (int i = 0; i < NCTRL; i++)
    g_dpu_conn->qp.recv(g_ctrl_mr, sizeof(uint64_t), i*sizeof(uint64_t), i);
#endif
}

void buddy_finalize()
{
  tt_print_mpi("buddy api breakdown");

  delete g_dpu_conn;
  g_dpu_conn = nullptr;
}

buddy_buf *buddy_alloc(size_t len)
{
  // TODO use huge pages to reduce TLB misses
  char *addr = new char[len];
  ibv_mr *mr = ibv_reg_mr(rdma::Context::get().get_pd(), addr, len,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  CHECK(mr);
  return mr;
}

void buddy_free(buddy_buf *buf)
{
  char *addr = (char *)buf->addr;
  CHECK(!ibv_dereg_mr(buf));
  delete[] addr;
}

void buddy_send(buddy_buf *buf, size_t len, size_t offset, uint64_t id)
{
  if (!len)
    return;

  //TicToc tt(TT_SEND);
  tic(TT_SEND);

  TRACE(1, "buddy_send len=" << len << " id=" << id);
  CHECK(len <= g_max_send);

#ifdef LOCAL_DMA
  // stage the message and send only the size; the proxy DMAs it. The control send's own
  // completion (wr_id=id) is what buddy_poll surfaces as the send completion.
  memcpy(g_dpu_conn->dma_buf->buf + DMA_OFFSET_SEND, (char *)buf->addr + offset, len);
  uint64_t sz = len;
  g_dpu_conn->qp.send_imm_inline(IMM_H2D_DMA, (char *)&sz, sizeof(sz), 0, id);
#else
  g_dpu_conn->qp.send_imm(IMM_H2D_RDMA, buf, len, offset, id);
#endif

  toc(TT_SEND);
}

void buddy_recv(buddy_buf *buf, size_t len, size_t offset, uint64_t id)
{
  //TicToc tt(TT_RECV);
  tic(TT_RECV);

  TRACE(1, "buddy_recv len=" << len << " id=" << id);
  CHECK(len >= g_min_recv);

#ifdef LOCAL_DMA
  g_pending_recvs.push({id, (char *)buf->addr + offset, len});   // filled from staging in buddy_poll
#else
  g_dpu_conn->qp.recv(buf, len, offset, id);
#endif

  toc(TT_RECV);
}

int buddy_poll(uint64_t *ids, size_t *sizes, int max)
{
  tic(TT_POLL);

  ibv_wc wc[max];
  assert(g_dpu_conn->qp.get_recv_cq() == g_dpu_conn->qp.get_send_cq());
  int n = ibv_poll_cq(g_dpu_conn->qp.get_recv_cq(), max, wc);

  if (n > 0)
    TRACE(1, "buddy_poll count=" << n);
  else
    TRACE(3, "buddy_poll count=" << n);

  if (n < 0) {
    perror("ibv_poll_cq");
    FAIL("ibv_poll_cq failed");
  }

  for (int i = 0; i < n; i++) {
#ifdef LOCAL_DMA
    if (wc[i].opcode & IBV_WC_RECV) {              // IMM_D2H_DMA: size + data in RECV staging
      uint64_t size;
      memcpy(&size, g_ctrl_buf + wc[i].wr_id*sizeof(uint64_t), sizeof(size));
      CHECK(!g_pending_recvs.empty());
      auto pr = g_pending_recvs.front(); g_pending_recvs.pop();
      CHECK(size <= pr.len);
      memcpy(pr.dst, g_dpu_conn->dma_buf->buf + DMA_OFFSET_RECV, size);
      ids[i] = pr.id;
      sizes[i] = size;
      g_dpu_conn->qp.recv(g_ctrl_mr, sizeof(uint64_t), wc[i].wr_id*sizeof(uint64_t), wc[i].wr_id);
    } else {                                       // IMM_H2D_DMA control sent -> app send done
      ids[i] = wc[i].wr_id;
      sizes[i] = 0;
    }
#else
    ids[i] = wc[i].wr_id;
    sizes[i] = wc[i].byte_len;
#endif

    TRACE(2, "buddy_poll id=" << ids[i] << " size=" << sizes[i]);

    if (wc[i].status != IBV_WC_SUCCESS) {
      std::cerr << "wc status " << wc[i].status << ": " << ibv_wc_status_str(wc[i].status) << std::endl;
      std::cerr << "vendor_err " << wc[i].vendor_err << std::endl;

      if (wc[i].status == IBV_WC_RNR_RETRY_EXC_ERR)
        std::cerr << "Maybe the DPU ran out of receive buffers or they are too small. (byte_len=" << wc[i].byte_len << ")" << std::endl;
      FAIL("wc error");
    }
  }

  toc(TT_POLL);

  return n;
}
