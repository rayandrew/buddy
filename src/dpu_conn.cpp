#include <iostream>
#include <unistd.h>
#include <assert.h>
#include <mpi.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "dpu_conn.h"
#include "sockets.h"
#include "local_proto.h"
#include "util.h"
#include "util_mpi.h"
#include "rdma.h"

namespace buddy::host {

uint32_t *make_address_table(const char *dpu_host, MPI_Comm leader_comm, MPI_Comm world_comm, int world_size)
{
  uint32_t dpu_ip;

  uint32_t *table = NULL;
  if (leader_comm != MPI_COMM_NULL)
    table = new uint32_t[world_size];

  struct hostent *dpu_ent = gethostbyname(dpu_host);
  CHECK(dpu_ent);
  CHECK(dpu_ent->h_addrtype == AF_INET);
  CHECK(dpu_ent->h_length == sizeof(dpu_ip));
  memcpy(&dpu_ip, *dpu_ent->h_addr_list, sizeof(dpu_ip));

  CHECK_MPI(MPI_Gather(&dpu_ip, sizeof(dpu_ip), MPI_BYTE, table, sizeof(dpu_ip), MPI_BYTE, 0, world_comm));
  CHECK_MPI(MPI_Bcast(table, sizeof(dpu_ip)*world_size, MPI_BYTE, 0, leader_comm));

  return table;
}

DpuConn::DpuConn(MPI_Comm world_comm)
{
  int world_rank;
  CHECK_MPI(MPI_Comm_rank(world_comm, &world_rank));

  // OpenMPI specific. In MPI-3, we could use MPI_COMM_TYPE_SHARED.
  // HACK: this does not work if world_comm != MPI_COMM_WORLD
  int local_size = 0;
  int local_rank = 0;
  char *env;

  env = getenv("OMPI_COMM_WORLD_LOCAL_SIZE");
  if (env)
    local_size = atoi(env);
  CHECK(local_size);

  env = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
  CHECK(env);
  local_rank = atoi(env);

  MPI_Comm leader_comm;
  CHECK_MPI(MPI_Comm_split(world_comm, local_rank == 0 ? 0 : MPI_UNDEFINED, 0, &leader_comm));

  char *host = getenv("BUDDY_DPU");
  CHECK(host && *host);

  int world_size = 0;
  CHECK_MPI(MPI_Comm_size(world_comm, &world_size));

  uint32_t *address_table = make_address_table(host, leader_comm, world_comm, world_size);
  if (local_rank == 0)
    assert(address_table);
  else
    assert(!address_table);

  int sock = tcp_connect(host, LOCAL_PORT);

  local_init msg = {
    .world_rank = world_rank,
    .world_size = world_size,
    .local_size = local_size,
#ifdef LOCAL_DMA
    .local_dma = true,
#else
    .local_dma = false,
#endif
    .send_address_table = !!address_table,
  };
  full_write(sock, (char *)&msg, sizeof(msg));

  if (address_table) {
    full_write(sock, (char *)address_table, sizeof(*address_table)*world_size);
    delete[] address_table;
  }

  new (&qp) rdma::QP(sock, true);

#ifdef LOCAL_DMA
  if (dma_buf)
    dma_buf->send(sock);
#endif

  char x;
  full_read(sock, &x, 1);

  close(sock);
}

DpuConn::~DpuConn()
{
  qp.write_imm(IMM_QUIT);
  ibv_wc wc;
  qp.wait_send(&wc);
  CHECK(wc.opcode == IBV_WC_RDMA_WRITE);
}

} // namespace buddy::host
