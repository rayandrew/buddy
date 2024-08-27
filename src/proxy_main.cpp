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

  buddy::rdma::init();

  int world_size = 0;
  int conn_count = 0;
  int host_recv_bufs = 0;
  int *conn_list = NULL;
  int *ranks = NULL;

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

    if (!world_size) {
      world_size = msg.world_size;
      std::cout << "world size = " << msg.world_size << std::endl;
      conn_list = new int[world_size];
      ranks = new int[world_size];
    } else
      CHECK(world_size == msg.world_size);

    if (!host_recv_bufs) {
      host_recv_bufs = msg.host_recv_bufs;
      std::cout << "num host recv buffers = " << msg.host_recv_bufs << std::endl;
    } else
      CHECK(host_recv_bufs == msg.host_recv_bufs);

    CHECK(msg.world_rank < world_size);

    conn_list[conn_count] = conn;
    ranks[conn_count] = msg.world_rank;

    conn_count++;
  } while (conn_count < world_size);

  close(lsock);

  buddy::rdma::server_cqs cqs;
  auto qps = new buddy::rdma::QP[conn_count];

  for (int i = 0; i < conn_count; i++)
    new (&qps[i]) buddy::rdma::QP(cqs, conn_list[i]);

#ifdef LOCAL_DMA
  buddy::dma::Engine dma_engine(conn_count, conn_list);
#endif

  buddy::dpu::ProxyConfig config;

  buddy::dpu::Proxy proxy(config, cqs, conn_count, qps,
#ifdef LOCAL_DMA
		  &dma_engine,
#endif
		  ranks, host_recv_bufs);

  char x = 0;
  for (int i = 0; i < conn_count; i++) {
    buddy::full_write(conn_list[i], &x, 1);
    close(conn_list[i]);
  }
  delete[] conn_list;

  proxy.rdma_loop();

  delete[] qps;
  cqs.destroy();
}
