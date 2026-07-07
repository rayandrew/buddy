#pragma once

#include <cstdint>
#include <cstddef>
#include <queue>

struct doca_dev;
struct doca_mmap;
struct doca_ctx;
struct doca_dma;
struct doca_buf_inventory;
struct doca_pe;
struct doca_buf;

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

class Engine {
  public:
    Engine(unsigned num_clients, int *socks);
    ~Engine();
    void transfer(jobspec job);
    bool poll(jobspec *job);

    char *client_buf(unsigned client)
    {
      return local_buf + client*buflen;
    }

    // called by the DOCA completion callback (ctx user data = this)
    void on_complete(const jobspec &j) { completed.push(j); }

  private:
    unsigned num_clients;
    size_t buflen;
    char *local_buf;
    char **remote_addr;

    doca_dev *dev;
    doca_ctx *ctx;
    doca_dma *dma_ctx;
    doca_pe *pe;
    doca_mmap *local_map;
    doca_buf_inventory *buf_inv;
    doca_mmap **remote_map;
    doca_buf **doca_buf_local;
    doca_buf **doca_buf_remote;
    std::queue<jobspec> completed;
};

} // namespace buddy::dma
