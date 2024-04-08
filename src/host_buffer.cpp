#include <cstdlib>
#include <cassert>
#include <cstring>
#include <mpi.h>
#include <absl/container/flat_hash_map.h>
#include "valgrind/memcheck.h"
#include "host_buffer.h"
#include "dpu_conn.h"
#include "util.h"
#include "rdma.h"
#include "dma.h"
#include "local_proto.h"

namespace buddy::host {

ReqBufWrite send_buf;
ReqBufRead recv_buf;

int world_rank;
int world_size;

DpuConn *dpu_conn;
dma::Buffer *dma_buf;

ibv_mr *recv_mr;

absl::flat_hash_map<recv_key, std::list<request>> recv_map;

void init()
{
  rdma::init();

  char *rdma_recv_buf = new char[RDMA_SIZE];
  recv_mr = ibv_reg_mr(rdma::Context::get().get_pd(), rdma_recv_buf, RDMA_SIZE,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  CHECK(recv_mr);

  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &world_size));

  dma_buf = new dma::Buffer(DMA_SIZE_TOTAL);

  send_buf = ReqBufWrite(dma_buf->buf + DMA_OFFSET_SEND, DMA_SIZE_SEND);
  recv_buf = ReqBufRead(dma_buf->buf + DMA_OFFSET_RECV, 0);

  dpu_conn = new DpuConn(world_rank, world_size, dma_buf);

  dpu_conn->qp.recv(recv_mr, RDMA_SIZE);
}

void finalize()
{
  delete dpu_conn;
  dpu_conn = nullptr;

  delete dma_buf;
  dma_buf = nullptr;

  char *addr = (char *)recv_mr->addr;
  CHECK(!ibv_dereg_mr(recv_mr));
  delete[] addr;
}

void put_send(request_head head, const char *data)
{
  while (!send_buf.append(head, data))
    flush();
}

void flush()
{
  if (send_buf.empty())
    return;

  uint64_t size = send_buf.get_pos();
  dpu_conn->qp.send_imm_inline(IMM_DMA_SEND_BUF, (char *)&size, sizeof(size));
  ibv_wc wc;
  dpu_conn->qp.wait_send(&wc);

  // Wait for DPU to finish transfer
  // Possible optimization: don't block here?
  dpu_conn->qp.wait_recv(&wc);
  CHECK(wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM);
  CHECK(wc.byte_len == 0);
  CHECK(wc.imm_data == IMM_DMA_SEND_BUF);

  dpu_conn->qp.recv(recv_mr, RDMA_SIZE);

  send_buf.reset_pos();
}

static bool try_recv(request_head *head, char *data)
{
  auto it = recv_map.find({head->src, head->tag});
  if (it == recv_map.end())
    return false;

  auto& list = it->second;
  assert(list.size());

  for (auto& req: list) {
    if (req.head.dst != world_rank)
      continue;

    CHECK(req.head.size <= head->size);
    memcpy(req.buf, data, head->size);
    req.head.dst = !world_rank;

    return true;
  }

  return false;
}

bool poll_recv()
{
  unsigned processed = 0;
  unsigned unprocessed = 0;

  if (recv_buf.empty()) {
    ibv_wc wc;
    if (dpu_conn->qp.poll_recv(&wc)) {
      uint64_t *size = (uint64_t *)recv_mr->addr;
      CHECK(wc.opcode == IBV_WC_RECV);
      CHECK(wc.byte_len == sizeof(*size));
      CHECK(wc.imm_data == IMM_DMA_RECV_BUF);
      recv_buf.reset_len(*size);

      VALGRIND_MAKE_MEM_DEFINED(dma_buf->buf + DMA_OFFSET_RECV, *size);

      dpu_conn->qp.recv(recv_mr, RDMA_SIZE);
    } else {
      return false;
    }
  }

  request_head *head;
  char *data;
  while (recv_buf.next(&head, &data, world_rank)) {
    if (try_recv(head, data)) {
      head->dst = !world_rank;
      processed++;
    } else {
      unprocessed++;
    }
  }

  if (unprocessed)
    recv_buf.reset_pos();
  else {
    // We finished with the buffer, ready to receive another
    recv_buf.reset_len(0);

    VALGRIND_MAKE_MEM_UNDEFINED(dma_buf->buf + DMA_OFFSET_RECV, DMA_SIZE_RECV);

    dpu_conn->qp.write_imm(IMM_DMA_RECV_BUF, NULL, NULL, 0, 0, 0, 0);
    // Optimization: dont need to block
    ibv_wc wc;
    dpu_conn->qp.wait_send(&wc);
  }

  return processed > 0;
}

recv_handle put_recv(request_head head, void *buf)
{
  recv_key key = {head.src, head.tag};
  auto it = recv_map.try_emplace(key);
  auto& list = it.first->second;

  request req = { .head = head, .buf = buf};
  list.push_back(req);

  return --list.end();
}

void delete_recv(recv_handle handle)
{
  auto list_it = recv_map.find({handle->head.src, handle->head.tag});
  assert(list_it != recv_map.end());
  auto& list = list_it->second;

  list.erase(handle);

  if (list.empty())
    recv_map.erase(list_it);
}

} // namespace buddy::host
