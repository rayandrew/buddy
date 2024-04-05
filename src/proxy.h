#pragma once

#include <absl/container/flat_hash_map.h>
#include <atomic>

#include "rdma.h"
#include "dma.h"
#include "request.h"

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
        rdma::QP *qps, dma::Engine *dma_engine, int *ranks);
    ~Proxy();
    void rdma_loop();

  private:
    const ProxyConfig config;
    rdma::server_cqs cqs;
    const unsigned num_clients;
    rdma::QP *qps;
    dma::Engine *dma_engine;
    std::atomic_bool quit;
    const size_t rx_depth;

    ibv_mr *mr;
    absl::flat_hash_map<unsigned, unsigned> qp_num_to_idx;
    absl::flat_hash_map<int, unsigned> rank_to_idx;
    ReqBufWrite *recv_bufs;

    void post_recv(uint64_t wr_id);

    void harvest_wcs();
    friend void *run_harvest_thread(void *arg);

    void route_reqs(char *buf, size_t len);
    void flush_dma();
};

} // namespace buddy::dpu
