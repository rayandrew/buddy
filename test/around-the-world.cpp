#include <iostream>
#include <cstring>
#include <mpi.h>
#include "util.h"

int main(int argc, char **argv)
{
  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  int status = 0;

  if (size < 2) {
    std::cerr << "Run with at least 2 ranks" << std::endl;
    status = 1;
  } else if (argc != 3) {
    if (rank == 0)
      std::cerr << "usage: around-the-world trips bytes" << std::endl;
    status = 1;
  } else {
    int trips = atoi(argv[1]);
    int bytes = atoi(argv[2]);

    char *msg = new char[bytes];
    memset(msg, 0, bytes);

    MPI_Request send_req, recv_req;
    if (rank == 0) {
      double t0 = MPI_Wtime();

      for (int i = 0; i < trips; i++) {
        CHECK_MPI(MPI_Isend(msg, bytes, MPI_BYTE, 1, 0, MPI_COMM_WORLD, &send_req));
        CHECK_MPI(MPI_Wait(&send_req, MPI_STATUS_IGNORE));

        CHECK_MPI(MPI_Irecv(msg, bytes, MPI_BYTE, size-1, 0, MPI_COMM_WORLD, &recv_req));
        CHECK_MPI(MPI_Wait(&recv_req, MPI_STATUS_IGNORE));
      }

      double t1 = MPI_Wtime();
      std::cout << t1-t0 << std::endl;
    } else {
      for (int i = 0; i < trips; i++) {
        CHECK_MPI(MPI_Irecv(msg, bytes, MPI_BYTE, rank-1, 0, MPI_COMM_WORLD, &recv_req));
        CHECK_MPI(MPI_Wait(&recv_req, MPI_STATUS_IGNORE));

        CHECK_MPI(MPI_Isend(msg, bytes, MPI_BYTE, (rank+1)%size, 0, MPI_COMM_WORLD, &send_req));
        CHECK_MPI(MPI_Wait(&send_req, MPI_STATUS_IGNORE));
      }
    }
  }

  CHECK_MPI(MPI_Finalize());
  return status;
}
