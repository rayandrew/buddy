#include <iostream>
#include <fstream>
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

uint32_t lookup_ip(const char *host)
{
  uint32_t ip;

  struct hostent *ent = gethostbyname(host);
  CHECK(ent);
  CHECK(ent->h_addrtype == AF_INET);
  CHECK(ent->h_length == sizeof(ip));
  memcpy(&ip, *ent->h_addr_list, sizeof(ip));

  return ip;
}

uint32_t *make_address_table(const char *dpu_host, MPI_Comm leader_comm, MPI_Comm world_comm, int world_size)
{
  uint32_t dpu_ip = lookup_ip(dpu_host);

  uint32_t *table = NULL;
  if (leader_comm != MPI_COMM_NULL) {
    table = new uint32_t[world_size];

    // The address in the table has to be reachable from other nodes
    int num_nodes;
    CHECK_MPI(MPI_Comm_size(leader_comm, &num_nodes));
    if (num_nodes > 1)
      CHECK(dpu_ip != lookup_ip("localhost"));
  }

  // Gather from all ranks
  CHECK_MPI(MPI_Gather(&dpu_ip, sizeof(dpu_ip), MPI_BYTE, table, sizeof(dpu_ip), MPI_BYTE, 0, world_comm));

  // Broadcast to leaders
  if (table)
    CHECK_MPI(MPI_Bcast(table, sizeof(dpu_ip)*world_size, MPI_BYTE, 0, leader_comm));

  return table;
}

DpuConn::DpuConn(MPI_Comm world_comm, int32_t h2d_size, int32_t d2h_size)
  : world_comm(world_comm)
{
  int world_rank;
  CHECK_MPI(MPI_Comm_rank(world_comm, &world_rank));

  // OpenMPI specific. In MPI-3, we could use MPI_COMM_TYPE_SHARED.
  // HACK: this does not work if world_comm != MPI_COMM_WORLD
  int local_size = 0;
  int local_rank = 0;

  /*
  char *env;

  env = getenv("OMPI_COMM_WORLD_LOCAL_SIZE");
  if (env)
    local_size = atoi(env);
  CHECK(local_size);

  env = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
  CHECK(env);
  local_rank = atoi(env);
  */
  MPI_Comm local_comm;
  CHECK_MPI(MPI_Comm_split_type(world_comm, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &local_comm));
  CHECK_MPI(MPI_Comm_size(local_comm, &local_size));
  CHECK_MPI(MPI_Comm_rank(local_comm, &local_rank));

  MPI_Comm leader_comm;
  CHECK_MPI(MPI_Comm_split(world_comm, local_rank == 0 ? 0 : MPI_UNDEFINED, 0, &leader_comm));

  char *host = getenv("BUDDY_DPU");
  if (!host || !*host) {
    const size_t maxlen = 256;
    std::ifstream fs("/usr/local/etc/buddy_dpu");
    host = new char[maxlen];
    fs.getline(host, maxlen);
  }
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
    .h2d_size = h2d_size,
    .d2h_size = d2h_size,
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

  // Wait for proxy to finish all setup
  char x;
  full_read(sock, &x, 1);

  close(sock);
}

DpuConn::~DpuConn()
{
  // QUIT is a collective operation since proxy exits immediately without
  // waiting or draining buffers.
  CHECK_MPI(MPI_Barrier(world_comm));
  qp.write_imm(IMM_QUIT);
  ibv_wc wc;
  qp.wait_send(&wc);
  CHECK(wc.opcode == IBV_WC_RDMA_WRITE);
}

} // namespace buddy::host
