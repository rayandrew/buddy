#pragma once

#include <absl/container/flat_hash_map.h>
#include <atomic>

#include "rdma.h"
#include "request.h"

#ifdef LOCAL_DMA
#include "dma.h"
#endif

#define PROXY_BUF_SIZE 2097152UL

struct doca_ctx;
struct doca_workq;
struct doca_buf_inventory;
struct doca_mmap;
struct doca_buf;

namespace buddy::dpu {

struct ProxyConfig {
};

class Proxy {
  public:
    Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients,
        rdma::QP *qps,
#ifdef LOCAL_DMA
        dma::Engine *dma_engine,
#endif
        int *ranks, int host_recv_bufs);
    ~Proxy();
    void rdma_loop();

  private:
    const ProxyConfig config;
    rdma::server_cqs cqs;
    const unsigned num_clients;
    rdma::QP *qps;
#ifdef LOCAL_DMA
    dma::Engine *dma_engine;
#endif
    std::atomic_bool quit;
    const size_t rx_depth;

    ibv_mr *h2d_mr;
    absl::flat_hash_map<unsigned, unsigned> qp_num_to_idx;
    absl::flat_hash_map<int, unsigned> rank_to_idx;
    int *idx_to_rank;

#ifndef LOCAL_DMA
    ibv_mr *d2h_mr;
#endif
    ReqBufWrite *d2h_bufs;
    int *client_recv_ready;
    int host_recv_bufs;

    void post_recv(uint64_t wr_id);

    void harvest_wcs();
    friend void *run_harvest_thread(void *arg);

    void route_reqs(char *buf, size_t len);
    bool try_flush_local();
};

} // namespace buddy::dpu
