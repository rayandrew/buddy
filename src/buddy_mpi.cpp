#include <iostream>
#include <mpi.h>
#include <dlfcn.h>
#include "util.h"

int MPI_Init(int *argc, char ***argv)
{
  static int(*real_MPI_Init)(int *argc, char ***argv);
  if(!real_MPI_Init)
    real_MPI_Init = (int(*)(int *argc, char ***argv)) dlsym(RTLD_NEXT, "MPI_Init");

  CHECK_MPI(real_MPI_Init(argc, argv));

  std::cout << "init" << std::endl;

  return 0;
}

int MPI_Irecv(void *buf, int count, MPI_Datatype datatype, int source, int tag,
    MPI_Comm comm, MPI_Request *request)
{
  std::cout << "hej in recv" << std::endl;
  std::cout << "wtime" << MPI_Wtime() << std::endl;
  return 0;
}

int MPI_Wait(MPI_Request *request, MPI_Status *status)
{
  return 0;
}

int MPI_Isend(const void *buf, int count, MPI_Datatype datatype, int dest, int tag,
    MPI_Comm comm, MPI_Request *request)
{
  return 0;
}
