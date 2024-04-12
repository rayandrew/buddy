#include <iostream>
#include <mpi.h>
#include "util.h"

int main(int argc, char **argv)
{
  CHECK(argc == 2);
  int max_iter = atoi(argv[1]);

  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  CHECK(size == 3);

  int recv[2] = {};
  MPI_Request req[4] = {};
  for (int i = 0; i < max_iter; i++) {
    if (rank == 0 || rank == 2) {
      CHECK_MPI(MPI_Irecv(recv+0, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, req+0));
      CHECK_MPI(MPI_Isend(&i, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, req+1));
      CHECK_MPI(MPI_Waitall(2, req, MPI_STATUSES_IGNORE));
    } else {
      CHECK_MPI(MPI_Irecv(recv+0, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, req+0));
      CHECK_MPI(MPI_Irecv(recv+1, 1, MPI_INT, 2, 0, MPI_COMM_WORLD, req+1));
      CHECK_MPI(MPI_Isend(&i, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, req+2));
      CHECK_MPI(MPI_Isend(&i, 1, MPI_INT, 2, 0, MPI_COMM_WORLD, req+3));
      CHECK_MPI(MPI_Waitall(4, req, MPI_STATUSES_IGNORE));
    }
  }

  CHECK_MPI(MPI_Finalize());
  return 0;
}
