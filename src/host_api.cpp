#include <iostream>
#include "buddy.h"
#include "host_buffer.h"
#include "dpu_conn.h"
#include "util.h"
#include "rdma.h"
#include "local_proto.h"

using namespace buddy;

static int g_trace;
#define TRACE(l,x) do{if (g_trace>=(l)){std::clog << x << std::endl;}}while(0)

static size_t g_max_send;
static size_t g_min_recv;

static host::DpuConn *g_dpu_conn;

void buddy_init(MPI_Comm comm, size_t max_send, size_t min_recv)
{
  rdma::init();

  char *env = getenv("BUDDY_TRACE");
  if (env)
    g_trace = atoi(env);

  g_max_send = max_send;
  g_min_recv = min_recv;

  g_dpu_conn = new host::DpuConn(comm, max_send, min_recv);
}

void buddy_finalize()
{
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

  TRACE(1, "buddy_send len=" << len << " id=" << id);
  CHECK(len <= g_max_send);

  g_dpu_conn->qp.send_imm(IMM_H2D_RDMA, buf, len, offset, id);
}

void buddy_recv(buddy_buf *buf, size_t len, size_t offset, uint64_t id)
{
  TRACE(1, "buddy_recv len=" << len << " id=" << id);
  CHECK(len >= g_min_recv);

  g_dpu_conn->qp.recv(buf, len, offset, id);
}

int buddy_poll(uint64_t *ids, size_t *sizes, int max)
{
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
    ids[i] = wc[i].wr_id;
    sizes[i] = wc[i].byte_len;

    TRACE(2, "buddy_poll id=" << ids[i] << " size=" << sizes[i]);

    if (wc[i].status != IBV_WC_SUCCESS) {
      std::cerr << "wc status " << wc[i].status << ": " << ibv_wc_status_str(wc[i].status) << std::endl;
      std::cerr << "vendor_err " << wc[i].vendor_err << std::endl;

      if (wc[i].status == IBV_WC_RNR_RETRY_EXC_ERR)
        std::cerr << "Maybe the DPU ran out of receive buffers or they are too small. (byte_len=" << wc[i].byte_len << ")" << std::endl;
      FAIL("wc error");
    }
  }

  return n;
}
