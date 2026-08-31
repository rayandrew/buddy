// Concurrency check for the shared doca_rdma progress engine: N threads submit and poll one
// DocaRdma, the way the proxy does once num_threads > 1.
//   server: ./rdma_doca_mt server <port> [threads] [msgs_per_thread]
//   client: ./rdma_doca_mt client <ip> <port> [threads] [msgs_per_thread]
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include "rdma_doca.h"
#include "sockets.h"

using namespace buddy;
static const size_t SLOT = 4096;

int main(int argc, char **argv)
{
  if (argc < 3) {
    std::cerr << "usage: server <port> [threads] [msgs] | client <ip> <port> [threads] [msgs]\n";
    return 2;
  }
  const bool server = !strcmp(argv[1], "server");
  auto arg = [&](int i, int dflt) {
    if (i >= argc || argv[i] == nullptr) return dflt;
    const int v = atoi(argv[i]);
    return v > 0 ? v : dflt;
  };
  const int nthreads = arg(server ? 3 : 4, 4);
  const int per = arg(server ? 4 : 5, 256);
  const int total = nthreads * per;

  // One slot per message on each side, so a receive is always posted and no send reuses a buffer
  // that is still in flight.
  const size_t slots = (size_t)total;
  const size_t len = 2 * slots * SLOT;
  char *mem = new char[len];
  memset(mem, 0, len);

  rdma::DocaRdma dr(1, mem, len);
  int sock = server ? accept(tcp_listen(atoi(argv[2])), NULL, NULL)
                    : tcp_connect(argv[2], atoi(argv[3]));
  dr.connect(0, sock, server);
  dr.wait_connected();

  std::atomic<int> done{0};
  std::atomic<int> bad{0};
  char x;

  if (server) {
    for (int i = 0; i < total; i++) dr.post_recv(slots * SLOT + (size_t)i * SLOT, SLOT, i);
    x = 1;
    full_write(sock, &x, 1);
  } else {
    full_read(sock, &x, 1);
  }

  std::vector<std::thread> th;
  for (int t = 0; t < nthreads; t++) {
    th.emplace_back([&, t] {
      rdma::DocaRdma::completion c;
      if (!server)
        for (int i = 0; i < per; i++) {
          const int id = t * per + i;
          memset(mem + (size_t)id * SLOT, (char)(id & 0xff), SLOT);
          dr.send_imm(0, (uint32_t)(id & 0xffff), (size_t)id * SLOT, SLOT, id);
          // Drain while producing: the send queue is finite and shared by every thread.
          while (dr.poll(&c)) done++;
        }
      // Counted globally, never per thread: any thread may drain any other thread's completion.
      while (done.load() < total)
        if (dr.poll(&c)) {
          if (server && (c.op != rdma::DocaRdma::OP_RECV ||
                         c.imm != (uint32_t)(c.wr_id & 0xffff)))
            bad++;
          done++;
        }
    });
  }
  for (auto &t : th) t.join();

  const bool ok = done.load() >= total && bad.load() == 0;
  std::cout << (server ? "server" : "client") << ": mt " << done.load() << "/" << total
            << " threads=" << nthreads << " mismatched=" << bad.load()
            << (ok ? "  OK" : "  FAIL") << std::endl;
  return ok ? 0 : 1;
}
