#include <cstdlib>
#include <cassert>
#include <cstring>
#include <mpi.h>
#include <absl/container/flat_hash_map.h>
#include "host_buffer.h"
#include "dpu_conn.h"
#include "util.h"
#include "rdma.h"
#include "dma.h"

namespace buddy::host {

char *send_buf;
size_t send_bytes;

char *recv_buf;
size_t recv_bytes;

int world_rank;
int world_size;

DpuConn dpu_conn;
dma::Buffer *dma_buf;

ibv_mr *recv_mr;

absl::flat_hash_map<recv_key, std::list<request>> recv_map;

void init()
{
  rdma::init();

  send_buf = (char *)malloc(SEND_BUFFER_SIZE);
  send_bytes = 0;

  recv_buf = (char *)malloc(RECV_BUFFER_SIZE);
  recv_bytes = 0;

  char *rdma_recv_buf = new char[RECV_BUFFER_SIZE];
  recv_mr = ibv_reg_mr(rdma::Context::get().get_pd(), rdma_recv_buf, RECV_BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  CHECK(recv_mr);

  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &world_size));

  dma_buf = new dma::Buffer(SEND_BUFFER_SIZE);

  new (&dpu_conn) DpuConn(world_rank, world_size, dma_buf);

  dpu_conn.qp.recv(recv_mr, RECV_BUFFER_SIZE);

}

void put_send(request_head head, const void *buf)
{
  size_t put_size = sizeof(head) + head.size;
  assert(put_size <= SEND_BUFFER_SIZE);

  if (send_bytes + put_size > SEND_BUFFER_SIZE) {
    flush();
  }

  memcpy(send_buf+send_bytes, &head, sizeof(head));
  memcpy(send_buf+send_bytes+sizeof(head), buf, head.size);

  send_bytes += put_size;
}

void flush()
{
  char hej[] = "hej!!";
  memcpy(dma_buf->buf, hej, sizeof(hej));
  size_t len = sizeof(hej);
  dpu_conn.qp.send_imm_inline(0, (char *)&len, sizeof(len));
  dpu_conn.qp.wait_op(IBV_WC_SEND);

  uint32_t rlen = dpu_conn.qp.wait_op(IBV_WC_RECV);
  CHECK(rlen == sizeof(uint64_t));

  uint64_t *dmalen = (uint64_t *)recv_mr->addr;
  std::cout << "Got msg from dpu " <<
    " dmalen = " << *dmalen << std::endl;
  char *str = dma_buf->buf;
  std::cout << "got: " << str << std::endl;

  if (send_bytes == 0)
    return;


  MPI_Send(&send_bytes, 1, MPI_UNSIGNED_LONG, !world_rank, 0, MPI_COMM_WORLD);
  MPI_Send(send_buf, send_bytes, MPI_BYTE, !world_rank, 0, MPI_COMM_WORLD);

  send_bytes = 0;
}

static bool try_recv(request_head *head, void *data)
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
  // Possible optimization: "merge" contiguous processed requests to reduce
  // number of hops in subsequent parses of the same buffer by increasing the
  // size of the first one.
  size_t pos = 0;
  unsigned processed = 0;
  unsigned unprocessed = 0;

  if (recv_bytes == 0) {
    MPI_Recv(&recv_bytes, 1, MPI_UNSIGNED_LONG, !world_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Recv(recv_buf, recv_bytes, MPI_BYTE, !world_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  }

  while (pos + sizeof(request_head) <= recv_bytes) {
    request_head *pos_head = reinterpret_cast<request_head*>(recv_buf+pos);

    // Already completed
    if (pos_head->dst != world_rank)
      continue;

    void *data = reinterpret_cast<void*>(pos_head+1);
    if (try_recv(pos_head, data)) {
      pos_head->dst = !world_rank;
      processed++;
    } else {
      unprocessed++;
    }

    pos += sizeof(request_head) + pos_head->size;
  }
  assert(pos == recv_bytes);

  if (!unprocessed) {
    std::cout << "emptied recv buffer" << std::endl;
    recv_bytes = 0;
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
