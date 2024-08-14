#include <iostream>
#include <mpi.h>
#include "util_mpi.h"

int main(int argc, char **argv)
{
  CHECK_MPI(MPI_Init(&argc, &argv));

  int rank, size;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &size));

  int status = 0;

  if (size != 2) {
    std::cerr << "Run with 2 ranks" << std::endl;
    status = 1;
  } else {
    MPI_Request req;
    int msg = 0;

    if (rank == 0) {
      CHECK_MPI(MPI_Irecv(&msg, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, &req));
      CHECK_MPI(MPI_Wait(&req, MPI_STATUS_IGNORE));
      std::cout << "hello! got msg " << msg << std::endl;

      CHECK_MPI(MPI_Barrier(MPI_COMM_WORLD));
    } else {
      msg = 123;
      CHECK_MPI(MPI_Isend(&msg, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, &req));
      CHECK_MPI(MPI_Wait(&req, MPI_STATUS_IGNORE));

      CHECK_MPI(MPI_Barrier(MPI_COMM_WORLD));
    }
  }

  CHECK_MPI(MPI_Finalize());
  return status;
}
