#include <iostream>
#include <cassert>
#include <mpi.h>
#include <dlfcn.h>
#include "util.h"
#include "host_buffer.h"

/* Communicator: for now assuming WORLD. We can do bypass on others probably */

/* Request: ompi is pointer, mpich is int. We can use pointer tagging (/sign?)
 * to distinguish between buddy requests and mpi requests. */

/* Message size: BUDDY_MAX_MESSAGE, and bypass to mpi on larger. */

static_assert(std::is_pointer<MPI_Request>());

int MPI_Init(int *argc, char ***argv)
{
  static int(*real_MPI_Init)(int *argc, char ***argv);
  if(!real_MPI_Init)
    real_MPI_Init = (int(*)(int *argc, char ***argv)) dlsym(RTLD_NEXT, "MPI_Init");

  CHECK_MPI(real_MPI_Init(argc, argv));

  buddy::host::init();

  return 0;
}

int MPI_Finalize()
{
  static int(*real_MPI_Finalize)();
  if(!real_MPI_Finalize)
    real_MPI_Finalize = (int(*)()) dlsym(RTLD_NEXT, "MPI_Finalize");

  buddy::host::flush();

  CHECK_MPI(real_MPI_Finalize());

  buddy::host::init();

  return 0;
}

int MPI_Irecv(void *buf, int count, MPI_Datatype datatype, int source, int tag,
    MPI_Comm comm, MPI_Request *mpi_req)
{
  assert(comm == MPI_COMM_WORLD);

  int ds = -1;
  CHECK_MPI(MPI_Type_size(datatype, &ds));

  CHECK(ds >= 0);
  CHECK(count >= 0);

  CHECK(source != MPI_ANY_SOURCE);
  CHECK(tag != MPI_ANY_TAG);

  auto req = new buddy::host::request();
  req->head.size = ds*count;
  req->head.rank = source;
  req->head.tag = tag;
  req->buf = buf;
  *mpi_req = reinterpret_cast<MPI_Request>(req);

  return 0;
}

int MPI_Isend(const void *buf, int count, MPI_Datatype datatype, int dest, int tag,
    MPI_Comm comm, MPI_Request *mpi_req)
{
  assert(comm == MPI_COMM_WORLD);

  int ds = 0;
  CHECK_MPI(MPI_Type_size(datatype, &ds));

  CHECK(count >= 0);
  CHECK(ds >= 0);

  buddy::request_head head = {
    .size = (size_t)ds*count,
    .rank = dest,
    .tag = tag,
  };
  buddy::host::put_send(head, buf);

  auto req_ptr = reinterpret_cast<buddy::host::request**>(mpi_req);
  *req_ptr = nullptr;

  return 0;
}

int MPI_Wait(MPI_Request *mpi_req, MPI_Status *status)
{
  auto req_ptr = reinterpret_cast<buddy::host::request**>(mpi_req);

  CHECK(status == MPI_STATUS_IGNORE);

  if (*req_ptr == nullptr)
    return 0;

  while (!buddy::host::try_recv((*req_ptr)->head, (*req_ptr)->buf))
    buddy::host::flush();

  delete *req_ptr;
  *req_ptr = nullptr;

  return 0;
}
