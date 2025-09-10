#include <mpi.h>
#include <buddy.h>
#include <request.h>
#include "util_mpi.h"

const size_t MAXLEN = 4*1024*1024;

enum {
  ID_SEND,
  ID_RECV,
};

void wait_for(uint64_t id, size_t size)
{
  for (;;) {
    uint64_t ids[2];
    uint64_t sizes[2];
    int n = buddy_poll(ids, sizes, 2);
    for (int j = 0; j < n; j++) {
      if (ids[j] == id) {
        CHECK(id != ID_RECV || sizes[j] == size);
        return;
      }
    }
  }
}

int main(int argc, char **argv)
{

  if (argc != 2){
    std::cerr << "Usage: " << argv[0] << " <max_message_size>\n";
    return 1;
  }

  const size_t messageSize = std::stoul(argv[1]);
  if (messageSize > MAXLEN - sizeof(buddy::request_head)) {
    std::cerr << "max_message_size must be <= " << MAXLEN - sizeof(buddy::request_head) << std::endl;
    return 1;
  }

  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  CHECK(size == 2);

  buddy_init(MPI_COMM_WORLD, MAXLEN, MAXLEN);

  buddy_buf *send_buf = buddy_alloc(MAXLEN);
  buddy_buf *recv_buf = buddy_alloc(MAXLEN);

  buddy::request_head head = {.size = (uint32_t)messageSize, .dst = !rank};
  memcpy(send_buf->addr, &head, sizeof(head));
  
  for (size_t i = 0; i < messageSize; i++)
    ((char *)send_buf->addr)[sizeof(head)+i] = 0xff;

  const int niter = 10000;
  double time[niter];

  if(rank == 0){
    std::cout << "Test buddy latency with message size " << messageSize << " bytes" << std::endl;
    std::cout << "min\tmax\tmedian\tmean\tp99" << std::endl;
  }

  for (int i = 0; i < niter; i++) {
    buddy_recv(recv_buf, MAXLEN, 0, ID_RECV);
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank == 0) {
      double t0 = MPI_Wtime();
      buddy_send(send_buf, sizeof(head)+messageSize, 0, ID_SEND);
      wait_for(ID_RECV, sizeof(head)+messageSize);
      double t1 = MPI_Wtime();
      time[i] = (t1-t0)/2;
    } else {
      wait_for(ID_RECV, sizeof(head)+messageSize);
      buddy_send(send_buf, sizeof(head)+messageSize, 0, ID_SEND);
      wait_for(ID_SEND, 0);
    }
  }

  if (rank == 0) {
    double min = time[0];
    double max = time[0];
    double mean = time[0];

    for (int i = 1; i < niter; i++) {
      double t = time[i];
      if (t < min)
        min = t;
      if (t > max)
        max = t;
      mean += t;
    }
    mean /= niter;

    std::sort(std::begin(time), std::end(time));
    double median = time[niter/2];
    double p99 = time[(unsigned)(niter*0.99)];

    std::cout << min << '\t' << max << '\t' << median << '\t' << mean << '\t' << p99 << std::endl;
  }

  buddy_free(recv_buf);
  buddy_free(send_buf);

  buddy_finalize();
  MPI_Finalize();
}
