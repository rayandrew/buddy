#include <unistd.h>
#include <sys/socket.h>
#include <cstdio>
#include <vector>
#include <limits.h>
#include "sockets.h"
#include "local_proto.h"
#include "util.h"
#include "util_mpi.h"
#include "rdma.h"
#include "proxy.h"

#ifdef LOCAL_DMA
#include <omp.h>
#include "dma.h"
#endif

int main(int argc, char **argv)
{
  CHECK_MPI(MPI_Init(NULL, NULL));

  int lsock = buddy::tcp_listen(buddy::LOCAL_PORT);
  int remote_lsock = buddy::tcp_listen(buddy::REMOTE_PORT);

  buddy::rdma::init();

  int local_size = 0;
  int conn_count = 0;
  int *conn_list = NULL;
  int *ranks = NULL;
  int world_size = 0;
  int local_min = INT_MAX;
  int local_max = 0;
  int h2d_size = 0;
  int d2h_size = 0;

  // DPU address for each rank, sent by local rank 0
  uint32_t *address_table = NULL;

  int rank;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));

  MPI_Barrier(MPI_COMM_WORLD);
  if (!rank)
    std::cout << "listening for connections..." << std::endl;

  do {
    int conn = accept(lsock, NULL, NULL);
    if(conn < 0) {
      perror("accept");
      FAIL("accept failed!");
    }

    buddy::local_init msg;
    buddy::full_read(conn, (char*)&msg, sizeof(msg));

#ifdef LOCAL_DMA
    CHECK(msg.local_dma);
#else
    CHECK(!msg.local_dma);
#endif

    if (!local_size) {
      local_size = msg.local_size;
      if (!rank)
        std::cout << "number of local ranks = " << msg.local_size << std::endl;
      conn_list = new int[local_size];
      ranks = new int[local_size];
    } else
      CHECK(local_size == msg.local_size);

    if (!world_size) {
      world_size = msg.world_size;
      if (!rank)
        std::cout << "number of total ranks = " << msg.world_size << std::endl;
    } else
      CHECK(world_size == msg.world_size);

    if (!h2d_size) {
      h2d_size = msg.h2d_size;
    } else
      CHECK(h2d_size == msg.h2d_size);

    if (!d2h_size) {
      d2h_size = msg.d2h_size;
    } else
      CHECK(d2h_size == msg.d2h_size);

    if (msg.send_address_table) {
      CHECK(!address_table);
      address_table = new uint32_t[msg.world_size];
      buddy::full_read(conn, (char *)address_table, world_size*sizeof(*address_table));
    }

    conn_list[conn_count] = conn;
    ranks[conn_count] = msg.world_rank;

    local_min = std::min(msg.world_rank, local_min);
    local_max = std::max(msg.world_rank, local_max);

    conn_count++;
  } while (conn_count < local_size);

  close(lsock);

  auto local_cqs = buddy::rdma::server_cqs::create();
  auto remote_cqs = local_cqs.duplicate_send();

  auto routing_table = new buddy::dpu::route[world_size];

  auto local_qps = new buddy::rdma::QP[conn_count];
  for (int i = 0; i < conn_count; i++) {
    new (&local_qps[i]) buddy::rdma::QP(local_cqs, conn_list[i]);
    routing_table[ranks[i]] = buddy::dpu::route::make_local(i);
  }

  int num_remotes = world_size / local_size - 1;
  for (int rank = 0; rank < world_size; rank++) {
    int dpu_id = rank / local_size;

    if (rank >= local_min) {
      if (rank <= local_max)
        continue;
      else
        dpu_id--;
    }

    assert(dpu_id < num_remotes);

    routing_table[rank] = buddy::dpu::route::make_remote(dpu_id);
  }

  // Assumption: ranks on same node are adjacent
  // Connect all DPU pairs
#ifdef DOCA_FABRIC
  int *rsock = new int[num_remotes]; bool *rsrv = new bool[num_remotes];   // deferred to DocaRdma
  buddy::rdma::QP *remote_qps = nullptr;
#else
  auto remote_qps = new buddy::rdma::QP[num_remotes];
#endif

  int local_node = local_min / local_size;
  for (int node = 0; node < local_node; node++) {
    int socket = buddy::tcp_connect_ip(address_table[node*local_size], buddy::REMOTE_PORT);

    buddy::full_write(socket, (char *)&local_node, sizeof(local_node));
    buddy::full_write(socket, (char *)&node, sizeof(node));

#ifdef DOCA_FABRIC
    rsock[node] = socket; rsrv[node] = false;
#else
    new (&remote_qps[node]) buddy::rdma::QP(remote_cqs, socket, false);
    close(socket);
#endif
  }

  for (int i = 0; i < num_remotes - local_node; i++) {
    int socket = accept(remote_lsock, NULL, NULL);

    int from_node, to_node;
    buddy::full_read(socket, (char *)&from_node, sizeof(from_node));
    buddy::full_read(socket, (char *)&to_node, sizeof(to_node));

    CHECK(to_node == local_node);
    CHECK(from_node > local_node);

#ifdef DOCA_FABRIC
    rsock[from_node-1] = socket; rsrv[from_node-1] = true;
#else
    new (&remote_qps[from_node-1]) buddy::rdma::QP(remote_cqs, socket, true);
    close(socket);
#endif
  }

  close(remote_lsock);

  delete[] address_table;
  address_table = nullptr;

#ifdef LOCAL_DMA
  buddy::dma::Engine dma_engine(conn_count, omp_get_max_threads(), conn_list);
#endif

  buddy::dpu::ProxyConfig config;
  char *env;
  env = getenv("BUDDY_TRACE");
  if (env && *env)
    config.trace = atoi(env);

  env = getenv("BUDDY_TIMEOUT");
  if (env && *env)
    config.timeout = strtod(env, NULL);

  env = getenv("BUDDY_QUIET_TIME");
  if (env && *env)
    config.quiet_time = strtod(env, NULL);

  env = getenv("BUDDY_D2D_SIZE");
  if (env && *env)
    config.d2d_size = atoi(env);

  env = getenv("BUDDY_BUFCOUNT_REMOTE");
  if (env && *env)
    config.bufcount_remote = atoi(env);

  env = getenv("BUDDY_BUFCOUNT_LOCAL");
  if (env && *env)
    config.bufcount_local = atoi(env);

  env = getenv("BUDDY_D2D_MODE");   // send (push, default) | read (pull) | write (one-sided)
  if (env && *env) {
    if (!strcmp(env, "send"))       config.d2d_mode = buddy::dpu::D2D_SEND;
    else if (!strcmp(env, "read"))  config.d2d_mode = buddy::dpu::D2D_READ;
    else if (!strcmp(env, "write")) config.d2d_mode = buddy::dpu::D2D_WRITE;
    else FAIL("BUDDY_D2D_MODE must be send|read|write, got " << env);
  }

  config.h2d_size = h2d_size;
  config.d2h_size = d2h_size;

  if (!rank) {
    std::cout << "=== proxy config ==" << std::endl;
    std::cout << config;
    std::cout << "===================" << std::endl;
  }

#ifdef DOCA_FABRIC
  // D2D over doca_rdma. fabric_mem = send half | recv half | ctrl (READY/ACK descriptors).
  size_t fabric_slots = (size_t)config.bufcount_remote * num_remotes;
  size_t fabric_len = 2 * fabric_slots * config.d2d_size + 2 * fabric_slots * 64;
  char *fabric_mem = new char[fabric_len];
  buddy::rdma::DocaRdma doca_fabric(num_remotes, fabric_mem, fabric_len);
  for (int i = 0; i < num_remotes; i++) { doca_fabric.connect(i, rsock[i], rsrv[i]); close(rsock[i]); }
  doca_fabric.wait_connected();
#endif

  buddy::dpu::proxy_cqs cqs = {
    .local_srq = local_cqs.srq,
    .remote_srq = remote_cqs.srq,
    .send = local_cqs.send,
    .local_recv = local_cqs.recv,
    .remote_recv = remote_cqs.recv,
  };

  buddy::dpu::Proxy proxy(config, cqs, conn_count, num_remotes, world_size,
      local_qps, remote_qps,
#ifdef LOCAL_DMA
      &dma_engine,
#endif
#ifdef DOCA_FABRIC
      &doca_fabric, fabric_mem,
#endif
      ranks, routing_table);

  char x = 0;
  for (int i = 0; i < conn_count; i++) {
    buddy::full_write(conn_list[i], &x, 1);
    close(conn_list[i]);
  }
  delete[] conn_list;

  proxy.rdma_loop();

  delete[] local_qps;
  delete[] remote_qps;

  CHECK(!ibv_destroy_cq(cqs.send));
  CHECK(!ibv_destroy_cq(cqs.local_recv));
  CHECK(!ibv_destroy_cq(cqs.remote_recv));
  CHECK(!ibv_destroy_srq(cqs.local_srq));
  CHECK(!ibv_destroy_srq(cqs.remote_srq));

  delete[] routing_table;

  MPI_Finalize();
}
