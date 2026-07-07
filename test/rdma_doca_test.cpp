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
  char x;
  auto wait = [&]{ while (!dr.poll(&c)) {} };
  int rc = 0;

  // (1) send/recv: client send_imm -> server recv
  if (server) {
    dr.post_recv(LEN, LEN, 77);
    x = 1; full_write(sock, &x, 1);
    wait();
    bool ok = c.op == rdma::DocaRdma::OP_RECV && c.imm == 0xABCD && c.len == LEN
              && (unsigned char)mem[LEN] == 0xC5;
    std::cout << "server: recv verify " << (ok ? "OK" : "FAIL") << std::endl; rc |= !ok;
  } else {
    full_read(sock, &x, 1);
    memset(mem, 0xC5, LEN);
    dr.send_imm(0, 0xABCD, 0, LEN, 55); wait();
  }

  // (2) read: server fills its send region, client reads it
  if (server) {
    memset(mem, 0xD7, LEN);                    // server send region
    x = 1; full_write(sock, &x, 1);
    full_read(sock, &x, 1);                     // wait until client done reading
  } else {
    full_read(sock, &x, 1);                     // wait until server filled
    dr.read(0, LEN, 0, LEN, 88); wait();        // read server[0..LEN) -> local recv region
    bool ok = c.op == rdma::DocaRdma::OP_READ && (unsigned char)mem[LEN] == 0xD7;
    std::cout << "client: read verify " << (ok ? "OK" : "FAIL") << std::endl; rc |= !ok;
    x = 1; full_write(sock, &x, 1);
  }

  // (3) write: client writes into server's recv region
  if (server) {
    full_read(sock, &x, 1);                     // wait until client wrote
    bool ok = (unsigned char)mem[LEN] == 0xE9;
    std::cout << "server: write verify " << (ok ? "OK" : "FAIL") << std::endl; rc |= !ok;
  } else {
    memset(mem, 0xE9, LEN);                     // client send region
    dr.write(0, 0, LEN, LEN, 99); wait();       // local[0..LEN) -> server recv region
    x = 1; full_write(sock, &x, 1);
  }

  return rc;
}
