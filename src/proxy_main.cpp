#include <unistd.h>
#include <sys/socket.h>
#include <cstdio>
#include "sockets.h"
#include "local_proto.h"
#include "util.h"
#include "rdma.h"
#include "proxy.h"

#ifdef LOCAL_DMA
#include "dma.h"
#endif

int main(int argc, char **argv)
{
  int lsock = buddy::tcp_listen(buddy::LOCAL_PORT);
  int remote_lsock = buddy::tcp_listen(buddy::REMOTE_PORT);

  buddy::rdma::init();

  int local_size = 0;
  int conn_count = 0;
  int *conn_list = NULL;
  int *ranks = NULL;
  uint32_t *address_table = NULL;
  int world_size = 0;
  int local_min = INT_MAX;
  int local_max = 0;
  int h2d_size = 0;
  int d2h_size = 0;

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
      std::cout << "number of local ranks = " << msg.local_size << std::endl;
      conn_list = new int[local_size];
      ranks = new int[local_size];
    } else
      CHECK(local_size == msg.local_size);

    if (!world_size) {
      world_size = msg.world_size;
      std::cout << "number of total ranks = " << msg.world_size << std::endl;
    } else
      CHECK(world_size == msg.world_size);

    if (!h2d_size) {
      h2d_size = msg.h2d_size;
      std::cout << "h2d_size " << msg.h2d_size << std::endl;
    } else
      CHECK(h2d_size == msg.h2d_size);

    if (!d2h_size) {
      d2h_size = msg.d2h_size;
      std::cout << "d2h_size " << msg.d2h_size << std::endl;
    } else
      CHECK(d2h_size == msg.d2h_size);

    if (msg.send_address_table) {
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

  // Assumption: ranks on same node are adjacent
  // Connect all DPU pairs
  std::vector<buddy::rdma::QP> remote_qps;

  for (int i = 0; i < world_size - local_size; i++) {
    int rank = (i + local_max + 1) % world_size;
    bool inverse = rank < local_min;

    if (rank == 0 || address_table[rank-1] != address_table[rank]) {
      int socket;
      if (inverse) {
        socket = buddy::tcp_connect_ip(address_table[rank], buddy::REMOTE_PORT);

        buddy::full_write(socket, (char *)&local_min, sizeof(local_min));
        buddy::full_write(socket, (char *)&rank, sizeof(rank));
      } else {
        socket = accept(remote_lsock, NULL, NULL);

        int msg;
        buddy::full_read(socket, (char *)&msg, sizeof(msg));
        CHECK(msg == rank);
        buddy::full_read(socket, (char *)&msg, sizeof(msg));
        CHECK(msg == local_min);
      }
      CHECK(socket >= 0);

      remote_qps.emplace_back(remote_cqs, socket, inverse);
      close(socket);
    }

    assert(remote_qps.size() > 0);
    routing_table[rank] = buddy::dpu::route::make_remote(remote_qps.size()-1);
  }

  close(remote_lsock);

  delete[] address_table;
  address_table = nullptr;

#ifdef LOCAL_DMA
  buddy::dma::Engine dma_engine(conn_count, conn_list);
#endif

  buddy::dpu::ProxyConfig config;
  char *env;
  env = getenv("BUDDY_TRACE");
  if (env)
    config.trace = atoi(env);

  env = getenv("BUDDY_TIMEOUT");
  if (env)
    config.timeout = strtod(env, NULL);

  config.h2d_size = h2d_size;
  config.d2h_size = d2h_size;

  buddy::dpu::proxy_cqs cqs = {
    .local_srq = local_cqs.srq,
    .remote_srq = remote_cqs.srq,
    .send = local_cqs.send,
    .local_recv = local_cqs.recv,
    .remote_recv = remote_cqs.recv,
  };

  buddy::dpu::Proxy proxy(config, cqs, conn_count, remote_qps.size(), world_size,
      local_qps, remote_qps.data(),
#ifdef LOCAL_DMA
      &dma_engine,
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
  remote_qps.clear();

  CHECK(!ibv_destroy_cq(cqs.send));
  CHECK(!ibv_destroy_cq(cqs.local_recv));
  CHECK(!ibv_destroy_cq(cqs.remote_recv));
  CHECK(!ibv_destroy_srq(cqs.local_srq));
  CHECK(!ibv_destroy_srq(cqs.remote_srq));

  delete[] routing_table;
}
