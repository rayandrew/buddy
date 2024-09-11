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

absl::flat_hash_map<recv_key, std::list<pending_recv>> pending_recv_map;

int trace;
#define TRACE(l,x) do{if (trace>=(l)){std::clog << x << std::endl;}}while(0)

std::ostream& operator<<(std::ostream& os, request_head& head)
{
  os << "(from:" << head.src << " to:" << head.dst << " tag:" << head.tag << " size:" << head.size << ")";
  return os;
}

std::ostream& operator<<(std::ostream& os, pending_recv& pr)
{
  os << "(size:" << pr.size << " src:" << pr.src << " tag:" << pr.tag << " real_size:" << pr.real_size << "real_src:" << pr.real_src << " real_tag:" << pr.real_tag << ")";
  return os;
}

void init()
{
  rdma::init();

  char *env = getenv("BUDDY_RECV_BUFS");
  if (env)
    num_recv_bufs = atoi(env);
  if (!num_recv_bufs)
    num_recv_bufs = 2;

  env = getenv("BUDDY_TRACE");
  if (env)
    trace = atoi(env);

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
  TRACE(2, "put_send " << head);

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

        TRACE(1, "d2h size " << size);

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

  TRACE(1, "flush size " << size);

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

static bool try_recv_from_list(request_head *head, char *data, std::list<pending_recv>& list)
{
  assert(list.size());

  for (auto& req: list) {
    if (req.completed())
      continue;

    CHECK(req.size >= head->size);
    memcpy(req.buf, data, head->size);

    assert(head->src >= 0);
    assert(head->tag >= 0);

    req.real_src = head->src;
    req.real_tag = head->tag;

    return true;
  }

  return false;
}

static bool try_recv(request_head *head, char *data)
{
  recv_key keys[] = {
    {head->src, head->tag},
    {MPI_ANY_SOURCE, MPI_ANY_TAG},
    {MPI_ANY_SOURCE, head->tag},
    {head->src, MPI_ANY_TAG},
  };

  for (size_t i = 0; i < sizeof(keys)/sizeof(*keys); i++) {
    auto it = pending_recv_map.find(keys[i]);
    if (it != pending_recv_map.end())
      if (try_recv_from_list(head, data, it->second))
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

      // Optimization: dont need to block
      ibv_wc wc;
      dpu_conn->qp.wait_send(&wc);
#else
      VALGRIND_MAKE_MEM_UNDEFINED((char *)recv_mr->addr + i*DMA_SIZE_RECV, DMA_SIZE_RECV);
      dpu_conn->qp.recv(recv_mr, DMA_SIZE_RECV, i*DMA_SIZE_RECV, i);
#endif
    }
  }

  return processed > 0;
}

recv_handle put_recv(request_head head, void *buf)
{
  TRACE(2, "put_recv " << head);

  recv_key key = {head.src, head.tag};
  auto it = pending_recv_map.try_emplace(key);
  auto& list = it.first->second;

  list.emplace_back(head, buf);

  return --list.end();
}

void complete_recv(recv_handle handle)
{
  TRACE(2, "complete_recv " << *handle);

  auto list_it = pending_recv_map.find({handle->src, handle->tag});
  assert(list_it != pending_recv_map.end());

  auto& list = list_it->second;
  list.erase(handle);

  if (list.empty())
    pending_recv_map.erase(list_it);
}

} // namespace buddy::host
