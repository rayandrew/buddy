#include <mpi.h>
#include <buddy.h>
#include <request.h>
#include "util_mpi.h"

const size_t MAXLEN = 1024*1024;

enum {
  ID_SEND,
  ID_RECV,
};

int main(int argc, char **argv)
{
  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  CHECK(size == 2);

  buddy_init(MPI_COMM_WORLD, MAXLEN, MAXLEN);

  buddy_buf *send_buf = buddy_alloc(MAXLEN);
  buddy_buf *recv_buf = buddy_alloc(MAXLEN);

  const int total = 10000000;
  int sent = 0;
  int recd = 0;

  struct msg {
    buddy::request_head head;
    int64_t i;
    double d;
  };

  msg *send_arr = (msg *)send_buf->addr;
  msg *recv_arr = (msg *)recv_buf->addr;

  size_t perbuf = MAXLEN / sizeof(msg);

  printf("perbuf %zu\n", perbuf);

  buddy_recv(recv_buf, MAXLEN, 0, ID_RECV);

  while (sent < total/2 || recd < total/2) {
    uint64_t id, size;
    int poll = buddy_poll(&id, &size, 1);
    if (poll && id == ID_RECV) {
      CHECK(size % sizeof(msg) == 0);
      size_t n = size / sizeof(msg);
      for (size_t i = 0; i < n; i++) {
        msg *m = recv_arr+i;
        WARN_EQ((double)m->i, m->d);
        //WARN_EQ(m->i, recd*2 + !rank);
        //CHECK_EQ(m->d, (double)(recd*2 + !rank));
        //printf("recd %d\n", recd*2+!rank);
        recd++;
      }
      buddy_recv(recv_buf, MAXLEN, 0, ID_RECV);
    } else if ((poll && id == ID_SEND) || sent == 0) {
      size_t i;
      for (i = 0; i < perbuf && sent < total/2; i++) {
        send_arr[i] = {
          .head = {
            .size = sizeof(msg) - sizeof(buddy::request_head),
            .dst = !rank,
          },
          .i = sent*2 + rank,
          .d = (double)(sent*2 + rank),
        };
        //printf("sent %d\n", sent*2+rank);
        sent++;
      }
      buddy_send(send_buf, i*sizeof(msg), 0, ID_SEND);
    } else if (poll) {
      FAIL("unhandled poll");
    }
  }

  MPI_Barrier(MPI_COMM_WORLD);

  buddy_free(recv_buf);
  buddy_free(send_buf);

  buddy_finalize();
  MPI_Finalize();

  if (rank == 0)
    printf("ok\n");
}
