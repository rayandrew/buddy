#include <cstdlib>
#include <cassert>
#include <cstring>
#include <mpi.h>
#include <absl/container/flat_hash_map.h>
#include "valgrind/memcheck.h"
#include "host_buffer.h"
#include "dpu_conn.h"
#include "util.h"
#include "util_mpi.h"
#include "rdma.h"
#include "local_proto.h"

#ifdef LOCAL_DMA
#include "dma.h"
#endif

namespace buddy::host {

ReqBufWrite send_buf;
bool hold_send_buf = false;

int num_recv_bufs;
ReqBufRead *recv_bufs;

int world_rank;

DpuConn *dpu_conn;
dma::Buffer *dma_buf;

ibv_mr *recv_mr;
ibv_mr *send_mr;

absl::flat_hash_map<recv_key, std::list<request>> recv_map;

void init()
{
  rdma::init();

  char *env = getenv("BUDDY_RECV_BUFS");
  if (env)
    num_recv_bufs = atoi(env);
  if (!num_recv_bufs)
    num_recv_bufs = 2;

  char *rdma_recv_buf = new char[num_recv_bufs*DMA_SIZE_RECV];
  recv_mr = ibv_reg_mr(rdma::Context::get().get_pd(), rdma_recv_buf, num_recv_bufs*DMA_SIZE_RECV,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  CHECK(recv_mr);

  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank));

#ifdef LOCAL_DMA
  dma_buf = new dma::Buffer(DMA_SIZE_TOTAL);

  send_buf = ReqBufWrite(dma_buf->buf + DMA_OFFSET_SEND, DMA_SIZE_SEND);
  recv_buf = ReqBufRead(dma_buf->buf + DMA_OFFSET_RECV, 0);
#else
  char *rdma_send_buf = new char[DMA_SIZE_SEND];
  send_mr = ibv_reg_mr(rdma::Context::get().get_pd(), rdma_send_buf, DMA_SIZE_RECV,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  CHECK(send_mr);

  send_buf = ReqBufWrite(rdma_send_buf, DMA_SIZE_SEND);
  recv_bufs = new ReqBufRead[num_recv_bufs];
  for (int i = 0; i < num_recv_bufs; i++)
    new (&recv_bufs[i]) ReqBufRead((char *)recv_mr->addr + i*DMA_SIZE_RECV, 0);
#endif

  dpu_conn = new DpuConn(world_rank, dma_buf, num_recv_bufs);

  for (int i = 0; i < num_recv_bufs; i++)
    dpu_conn->qp.recv(recv_mr, DMA_SIZE_RECV, i*DMA_SIZE_RECV, i);
}

void finalize()
{
  delete dpu_conn;
  dpu_conn = nullptr;

#ifdef LOCAL_DMA
  delete dma_buf;
  dma_buf = nullptr;
#endif

  char *addr;
  addr = (char *)recv_mr->addr;
  CHECK(!ibv_dereg_mr(recv_mr));
  delete[] addr;

  if (send_mr) {
    addr = (char *)send_mr->addr;
    CHECK(!ibv_dereg_mr(send_mr));
    delete[] addr;
  }

  delete[] recv_bufs;
}

void put_send(request_head head, const char *data)
{
  while (!send_buf.append(head, data))
    flush();
}

static bool poll_rdma_recv()
{
  ibv_wc wc;
  if (!dpu_conn->qp.poll_recv(&wc))
    return false;

  switch (wc.imm_data) {
    case IMM_H2D_DMA:
      {
#ifndef LOCAL_DMA
        FAIL("unexpected dma message");
#endif

        CHECK(wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM);
        CHECK(wc.byte_len == 0);

        hold_send_buf = false;
        send_buf.reset_pos();

        break;
      }

    case IMM_D2H_DMA:
      {
#ifndef LOCAL_DMA
        FAIL("unexpected dma message");
#else

        uint64_t *size = (uint64_t *)recv_mr->addr;
        CHECK(wc.opcode == IBV_WC_RECV);
        CHECK(wc.byte_len == sizeof(*size));

        recv_buf.reset_len(*size);
        VALGRIND_MAKE_MEM_DEFINED(dma_buf->buf + DMA_OFFSET_RECV, *size);
#endif

        break;
      }

    case IMM_D2H_RDMA:
      {
        uint64_t size = wc.byte_len;
        CHECK(wc.opcode == IBV_WC_RECV);

        recv_bufs[wc.wr_id].reset_len(size);
        VALGRIND_MAKE_MEM_DEFINED((char *)recv_mr->addr + wc.wr_id*DMA_SIZE_RECV, size);

        break;
      }

    default:
      {
        FAIL("unknown imm_data " << wc.imm_data);
      }
  }

#ifdef LOCAL_DMA
  dpu_conn->qp.recv(recv_mr, DMA_SIZE_RECV);
#endif

  return true;
}

void flush()
{
  if (send_buf.empty())
    return;

  assert(!hold_send_buf);
  hold_send_buf = true;

  uint64_t size = send_buf.get_pos();
#ifdef LOCAL_DMA
  dpu_conn->qp.send_imm_inline(IMM_H2D_DMA, (char *)&size, sizeof(size));
#else
  dpu_conn->qp.send_imm(IMM_H2D_RDMA, send_mr, size);
#endif

  ibv_wc wc;
  dpu_conn->qp.wait_send(&wc);

#ifdef LOCAL_DMA
  // Wait for DPU to finish transfer
  // Possible optimization: don't block here?
  while (hold_send_buf)
    poll_rdma_recv();
#else
  // In rdma mode, transfer is completed after wait_send
  hold_send_buf = false;
  send_buf.reset_pos();
#endif

  assert(send_buf.empty());
  assert(!hold_send_buf);
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

  for (int i = 0; i < num_recv_bufs; i++) {
    unsigned unprocessed = 0;

    if (recv_bufs[i].empty()) {
      poll_rdma_recv();
      if (recv_bufs[i].empty())
        continue;
    }

    request_head *head;
    char *data;
    while (recv_bufs[i].next(&head, &data, world_rank)) {
      if (try_recv(head, data)) {
        head->dst = !world_rank;
        processed++;
      } else {
        unprocessed++;
      }
    }

    if (unprocessed)
      recv_bufs[i].reset_pos();
    else {
      // We finished with the buffer, ready to receive another
      recv_bufs[i].reset_len(0);

#ifdef LOCAL_DMA
      VALGRIND_MAKE_MEM_UNDEFINED(dma_buf->buf + DMA_OFFSET_RECV, DMA_SIZE_RECV);
      dpu_conn->qp.write_imm(IMM_D2H_DMA, NULL, NULL, 0, 0, 0, 0);
#else

      VALGRIND_MAKE_MEM_UNDEFINED((char *)recv_mr->addr + i*DMA_SIZE_RECV, DMA_SIZE_RECV);
      dpu_conn->qp.recv(recv_mr, DMA_SIZE_RECV, i*DMA_SIZE_RECV, i);
      dpu_conn->qp.write_imm(IMM_D2H_RDMA, NULL, NULL, 0, 0, 0, 0);
#endif

      // Optimization: dont need to block
      ibv_wc wc;
      dpu_conn->qp.wait_send(&wc);
    }
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
