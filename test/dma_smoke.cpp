// Minimal host<->DPU DOCA DMA smoke test for buddy::dma (Buffer + Engine, DOCA 3.0).
// Proves the local-transport engine moves data both directions before wiring it into the proxy.
//   host: ./dma_smoke host <port>              (BUDDY_HOST_PCI=<pci>)
//   dpu:  ./dma_smoke dpu  <host_ip> <port>    (BUDDY_DPU_PCI=<pci>)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>

#include "dma.h"
#include "sockets.h"

using namespace buddy;

static const size_t LEN = 1 << 20;   // 1 MiB

int main(int argc, char **argv)
{
  if (argc < 3) { std::cerr << "usage: host <port> | dpu <host> <port>\n"; return 2; }
  std::string mode = argv[1];

  if (mode == "host") {
    int lsock = tcp_listen(atoi(argv[2]));
    dma::Buffer b(LEN);
    for (size_t i = 0; i < LEN; i++) b.buf[i] = (char)(i & 0xff);   // H2D pattern
    int conn = accept(lsock, NULL, NULL);
    if (conn < 0) { perror("accept"); return 1; }
    b.send(conn);                                                   // export to DPU
    char x; full_read(conn, &x, 1);                                 // wait for DPU's D2H
    bool ok = true;
    for (int i = 0; i < 16; i++) if ((unsigned char)b.buf[i] != 0xAB) ok = false;
    std::cout << "host: D2H verify " << (ok ? "OK" : "FAIL") << std::endl;
    return ok ? 0 : 1;
  }

  // dpu
  int sock = tcp_connect(argv[2], atoi(argv[3]));
  dma::Engine eng(1, 1, &sock);                                    // 1 client, 1 worker
  dma::jobspec done;

  eng.transfer(0, {0, 0, (uint32_t)LEN, dma::H2D});                 // pull host -> local
  while (!eng.poll(0, &done)) {}
  char *lb = eng.client_buf(0);
  bool ok = true;
  for (size_t i = 0; i < LEN; i++) if ((unsigned char)lb[i] != (i & 0xff)) { ok = false; break; }
  std::cout << "dpu: H2D verify " << (ok ? "OK" : "FAIL") << std::endl;

  for (int i = 0; i < 16; i++) lb[i] = (char)0xAB;                  // D2H pattern
  eng.transfer(0, {0, 0, 16, dma::D2H});                           // push local -> host
  while (!eng.poll(0, &done)) {}
  char x = 1; full_write(sock, &x, 1);
  std::cout << "dpu: D2H done" << std::endl;
  return ok ? 0 : 1;
}
