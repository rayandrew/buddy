#include <unistd.h>
#include <sys/socket.h>
#include <cstdio>
#include "sockets.h"
#include "local_proto.h"
#include "util.h"
#include "rdma.h"
#include "proxy.h"
#include "dma.h"

int main(int argc, char **argv)
{
  int lsock = buddy::tcp_listen(buddy::LOCAL_PORT);

  buddy::rdma::init();

  int world_size = 0;
  int conn_count = 0;
  int *conn_list = NULL;

  do {
    int conn = accept(lsock, NULL, NULL);
    if(conn < 0) {
      perror("accept");
      FAIL("accept failed!");
    }

    buddy::local_init msg;
    buddy::full_read(conn, (char*)&msg, sizeof(msg));

    if (!world_size) {
      world_size = msg.world_size;
      std::cout << "world size = " << msg.world_size << std::endl;
      conn_list = new int[world_size];
    } else
      CHECK(world_size == msg.world_size);

    CHECK(msg.world_rank < world_size);

    conn_count++;
    conn_list[msg.world_rank] = conn;
  } while (conn_count < world_size);

  close(lsock);

  buddy::rdma::server_cqs cqs;
  auto qps = new buddy::rdma::QP[conn_count];

  for (int i = 0; i < conn_count; i++) {
    new (&qps[i]) buddy::rdma::QP(cqs, conn_list[i]);
  }

  buddy::dma::Engine dma_engine(conn_count, conn_list);

  buddy::dpu::ProxyConfig config;

  buddy::dpu::Proxy proxy(config, cqs, conn_count, qps, &dma_engine);
  proxy.rdma_loop();
}
