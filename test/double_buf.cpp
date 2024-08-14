#include <iostream>
#include <mpi.h>
#include "util_mpi.h"

int main(int argc, char **argv)
{
  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  CHECK(size == 3);

  MPI_Request req[2];
  int msg = 0;

  if (rank == 0) {
    for (int i = 0; i < 2; i++) {
      CHECK_MPI(MPI_Isend(&msg, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, req+0));
      CHECK_MPI(MPI_Wait(req, MPI_STATUS_IGNORE));
    }
  }

  MPI_Barrier(MPI_COMM_WORLD);

  if (rank == 1) {
    for (int i = 0; i < 2; i++) {
      CHECK_MPI(MPI_Irecv(&msg, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, req+0));
      CHECK_MPI(MPI_Irecv(&msg, 1, MPI_INT, 2, 0, MPI_COMM_WORLD, req+1));
      CHECK_MPI(MPI_Waitall(2, req, MPI_STATUS_IGNORE));
    }
  }
  
  if (rank == 2) {
    for (int i = 0; i < 2; i++) {
      CHECK_MPI(MPI_Isend(&msg, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, req+0));
      CHECK_MPI(MPI_Wait(req, MPI_STATUS_IGNORE));
    }
  }

  CHECK_MPI(MPI_Finalize());
  return 0;
}
