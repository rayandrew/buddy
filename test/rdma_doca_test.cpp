// Validate the DocaRdma D2D class: server post_recv, client send_imm, verify IMM + data.
//   server: ./rdma_doca_test server <port>
//   client: ./rdma_doca_test client <ip> <port>   (BUDDY_RDMA_DEV=mlx5_2)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include "rdma_doca.h"
#include "sockets.h"

using namespace buddy;
static const size_t LEN = 4096;

int main(int argc, char **argv)
{
  if (argc < 3) { std::cerr << "usage: server <port> | client <ip> <port>\n"; return 2; }
  bool server = !strcmp(argv[1], "server");
  char *mem = new char[2*LEN];                 // [0,LEN)=send region, [LEN,2LEN)=recv region
  memset(mem, 0, 2*LEN);

  rdma::DocaRdma dr(1, mem, 2*LEN);
  int sock = server ? accept(tcp_listen(atoi(argv[2])), NULL, NULL)
                    : tcp_connect(argv[2], atoi(argv[3]));
  dr.connect(0, sock, server);
  dr.wait_connected();

  rdma::DocaRdma::completion c;
  if (server) {
    dr.post_recv(LEN, LEN, 77);                // recv into recv region, wr_id 77
    char x = 1; full_write(sock, &x, 1);       // tell client recv is posted
    while (!dr.poll(&c)) {}
    bool ok = c.is_recv && c.wr_id == 77 && c.imm == 0xABCD && c.len == LEN
              && (unsigned char)mem[LEN] == 0xC5;
    std::cout << "server: recv imm=" << std::hex << c.imm << std::dec << " len=" << c.len
              << " verify " << (ok ? "OK" : "FAIL") << std::endl;
    return ok ? 0 : 1;
  }
  char x; full_read(sock, &x, 1);              // wait until recv posted
  memset(mem, 0xC5, LEN);                       // send region
  dr.send_imm(0, 0xABCD, 0, LEN, 55);          // send region -> peer, imm 0xABCD
  while (!dr.poll(&c)) {}
  std::cout << "client: send done wr_id=" << c.wr_id << std::endl;
  return 0;
}
