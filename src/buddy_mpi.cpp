#include <iostream>
#include <cassert>
#include <mpi.h>
#include "util.h"
#include "util_mpi.h"
#include "host_buffer.h"

/* Communicator: for now assuming WORLD. We can do bypass on others probably */

/* Request: ompi is pointer, mpich is int. We can use pointer tagging (/sign?)
 * to distinguish between buddy requests and mpi requests. */

/* Message size: BUDDY_MAX_MESSAGE, and bypass to mpi on larger. */

static_assert(std::is_pointer<MPI_Request>());
static_assert(sizeof(buddy::host::recv_handle) == sizeof(void *));

int world_rank;

int MPI_Init(int *argc, char ***argv)
{
  CHECK_MPI(PMPI_Init(argc, argv));
  CHECK_MPI(PMPI_Comm_rank(MPI_COMM_WORLD, &world_rank));

  buddy::host::init();

  return 0;
}

int MPI_Finalize()
{
  buddy::host::flush();
  
  PMPI_Barrier(MPI_COMM_WORLD);
  buddy::host::finalize();

  CHECK_MPI(PMPI_Finalize());

  return 0;
}

int MPI_Irecv(void *buf, int count, MPI_Datatype datatype, int source, int tag,
    MPI_Comm comm, MPI_Request *mpi_req)
{
  assert(comm == MPI_COMM_WORLD);

  int ds = -1;
  CHECK_MPI(PMPI_Type_size(datatype, &ds));

  CHECK(ds >= 0);
  CHECK(count >= 0);

  buddy::request_head head = {
    .size = (size_t)ds*count,
    .src = source,
    .dst = world_rank,
    .tag = tag,
  };
  auto handle = buddy::host::put_recv(head, buf);

  // Evil hack to cast handle (ie iterator) to MPI_Request (ie pointer)
  auto handle_ptr = &handle;
  *mpi_req = *((MPI_Request*)handle_ptr);

  return 0;
}

int MPI_Isend(const void *buf, int count, MPI_Datatype datatype, int dest, int tag,
    MPI_Comm comm, MPI_Request *mpi_req)
{
  assert(comm == MPI_COMM_WORLD);

  int ds = 0;
  CHECK_MPI(PMPI_Type_size(datatype, &ds));

  CHECK(count >= 0);
  CHECK(ds >= 0);

  buddy::request_head head = {
    .size = (size_t)ds*count,
    .src = world_rank,
    .dst = dest,
    .tag = tag,
  };
  buddy::host::put_send(head, (const char *)buf);

  *mpi_req = MPI_REQUEST_NULL;

  return 0;
}

int MPI_Wait(MPI_Request *mpi_req, MPI_Status *status)
{
  if (*mpi_req == MPI_REQUEST_NULL)
    return 0;

  auto handle = *reinterpret_cast<buddy::host::recv_handle*>(mpi_req);

  // dst field indicates if request is completed
  while (handle->head.dst == world_rank)
    while (!buddy::host::poll_recv())
      buddy::host::flush();

  if (status != MPI_STATUS_IGNORE) {
    status->MPI_SOURCE = handle->head.src;
    status->MPI_TAG = handle->head.tag;
    status->MPI_ERROR = MPI_SUCCESS;
  }

  buddy::host::delete_recv(handle);

  *mpi_req = MPI_REQUEST_NULL;

  return 0;
}

int MPI_Waitall(int count, MPI_Request mpi_reqs[], MPI_Status statuses[])
{
  for (int i = 0; i < count; i++)
    if (statuses == MPI_STATUSES_IGNORE)
      MPI_Wait(mpi_reqs+i, MPI_STATUS_IGNORE);
    else
      MPI_Wait(mpi_reqs+i, statuses+i);

  return 0;
}

int MPI_Barrier(MPI_Comm comm)
{
  assert(comm == MPI_COMM_WORLD);

  // Required for test write_barrier to avoid deadlock
  buddy::host::flush();

  return PMPI_Barrier(comm);
}
