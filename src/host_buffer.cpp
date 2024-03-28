#include <cstdlib>
#include <cassert>
#include <cstring>
#include <mpi.h>
#include "host_buffer.h"
#include "dpu_conn.h"
#include "util.h"

namespace buddy::host {

char *send_buf;
size_t send_bytes;

char *recv_buf;
size_t recv_bytes;

int world_rank;
int world_size;

DpuConn dpu_conn;

void init()
{
  send_buf = (char *)malloc(SEND_BUFFER_SIZE);
  send_bytes = 0;

  recv_buf = (char *)malloc(RECV_BUFFER_SIZE);
  recv_bytes = 0;

  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &world_rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &world_size));

  dpu_conn = DpuConn(world_rank, world_size);
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
  if (send_bytes == 0)
    return;

  MPI_Send(&send_bytes, 1, MPI_UNSIGNED_LONG, !world_rank, 0, MPI_COMM_WORLD);
  MPI_Send(send_buf, send_bytes, MPI_BYTE, !world_rank, 0, MPI_COMM_WORLD);

  send_bytes = 0;
}

bool try_recv(request_head head, void *buf)
{
  size_t size = sizeof(head) + head.size;
  size_t pos = 0;

  if (recv_bytes == 0) {
    MPI_Recv(&recv_bytes, 1, MPI_UNSIGNED_LONG, !world_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Recv(recv_buf, recv_bytes, MPI_BYTE, !world_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  }

  while (pos + size <= recv_bytes) {
    request_head *pos_head = reinterpret_cast<request_head*>(recv_buf+pos);

    if (pos_head->rank == world_rank) {
      if (pos_head->tag == head.tag) {
        CHECK(pos_head->size <= head.size);

        void *data = reinterpret_cast<void*>(pos_head+1);
        memcpy(buf, data, pos_head->size);

        // Mark request as processed
        pos_head->rank = !world_rank;

        return true;
      }
    }

    pos += sizeof(head) + pos_head->size;
  }

  return false;
}

} // namespace buddy::host
