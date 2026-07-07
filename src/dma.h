#pragma once

#include <cstdint>
#include <cstddef>

struct doca_dev;
struct doca_mmap;

namespace buddy::dma {

// Host<->DPU local transport over DOCA DMA (DOCA 3.0: doca_pe + tasks + export_pci).
// PCI addresses come from BUDDY_HOST_PCI / BUDDY_DPU_PCI (see dma.cpp defaults).

class Buffer {
  public:
    Buffer(size_t len);
    ~Buffer();
    void send(int sockfd);   // export this host buffer to the DPU over a socket

    char * const buf;

  private:
    size_t len;

    doca_dev *dev;
    doca_mmap *dmap;
};

enum direction { H2D, D2H };

struct jobspec {
  unsigned client;
  uint32_t offset;
  uint32_t len;
  direction dir;
};

// One DOCA progress engine + DMA context per worker (proxy thread), so workers DMA in
// parallel. Device and memory mappings are shared; workers[] is defined in dma.cpp.
struct dma_worker;

class Engine {
  public:
    Engine(unsigned num_clients, unsigned num_workers, int *socks);
    ~Engine();
    void transfer(unsigned worker, jobspec job);
    bool poll(unsigned worker, jobspec *job);

    char *client_buf(unsigned client) { return local_buf + client*buflen; }

  private:
    unsigned num_clients;
    unsigned num_workers;
    size_t buflen;
    char *local_buf;
    char **remote_addr;

    doca_dev *dev;
    doca_mmap *local_map;
    doca_mmap **remote_map;
    dma_worker *workers;
};

} // namespace buddy::dma
