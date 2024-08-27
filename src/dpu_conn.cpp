#include <iostream>
#include <unistd.h>
#include <assert.h>
#include "dpu_conn.h"
#include "sockets.h"
#include "local_proto.h"
#include "util.h"
#include "rdma.h"

namespace buddy::host {

DpuConn::DpuConn(int world_rank, int world_size, dma::Buffer *dma_buf, int host_recv_bufs)
{
  char *host = getenv("BUDDY_DPU");
  CHECK(host && *host);

  int sock = tcp_connect(host, LOCAL_PORT);

  local_init msg = {
    .world_rank = world_rank,
    .world_size = world_size,
#ifdef LOCAL_DMA
    .local_dma = true,
#else
    .local_dma = false,
#endif
    .host_recv_bufs = host_recv_bufs,
  };
  full_write(sock, (char *)&msg, sizeof(msg));

  new (&qp) rdma::QP(sock);

#ifdef LOCAL_DMA
  if (dma_buf)
    dma_buf->send(sock);
#else
  assert(!dma_buf);
#endif

  char x;
  full_read(sock, &x, 1);

  close(sock);
}

DpuConn::~DpuConn()
{
  qp.write_imm(IMM_QUIT);
  ibv_wc wc;
  qp.wait_send(&wc);
  CHECK(wc.opcode == IBV_WC_RDMA_WRITE);
}

} // namespace buddy::host
