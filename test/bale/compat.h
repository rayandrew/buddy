#pragma once

#include <mpi.h>
#include <util_mpi.h>

#define T0_fprintf(fp, ...) do{if (!MYTHREAD) fprintf((fp), __VA_ARGS__);}while(0);
#define T0_printf(...) do{if (!MYTHREAD) printf(__VA_ARGS__);}while(0);

#define lgp_finalize() MPI_Finalize()
#define lgp_global_exit(x) MPI_Abort(MPI_COMM_WORLD, x)
#define lgp_barrier() MPI_Barrier(MPI_COMM_WORLD)

#define wall_seconds() MPI_Wtime()

extern int THREADS;
extern int MYTHREAD;

inline int64_t lgp_reduce_add_l(int64_t x)
{
  int64_t y;
  CHECK_MPI(MPI_Allreduce(&x, &y, 1, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD));
  return y;
}

inline double lgp_reduce_add_d(double x)
{
  double y;
  CHECK_MPI(MPI_Allreduce(&x, &y, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD));
  return y;
}

inline int64_t lgp_reduce_max_l(int64_t x)
{
  int64_t y;
  CHECK_MPI(MPI_Allreduce(&x, &y, 1, MPI_INT64_T, MPI_MAX, MPI_COMM_WORLD));
  return y;
}

inline void lgp_init(int argc, char **argv)
{
  CHECK_MPI(MPI_Init(&argc, &argv));
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &MYTHREAD));
  CHECK_MPI(MPI_Comm_size(MPI_COMM_WORLD, &THREADS));
}
