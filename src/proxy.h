#pragma once

#include <absl/container/flat_hash_map.h>
#include <atomic>

#include "rdma.h"
#include "request.h"
#include "util.h"

#ifdef LOCAL_DMA
#include "dma.h"
#endif

#define PROXY_BUF_SIZE 2097152UL

namespace buddy::dpu {

struct ProxyConfig {
};

struct route {
  uint32_t remote : 1;
  uint32_t idx : 31;

  static inline route make_local(int32_t idx)
  {
    route r = { .remote = 0, .idx = (uint32_t)idx };
    CHECK(r.idx == idx);
    return r;
  }

  static inline route make_remote(int32_t idx)
  {
    route r = { .remote = 1, .idx = (uint32_t)idx };
    CHECK(r.idx == idx);
    return r;
  }
};

class Proxy {
  public:
    Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients, int world_size,
        rdma::QP *local_qps, rdma::QP *remote_qps,
#ifdef LOCAL_DMA
        dma::Engine *dma_engine,
#endif
        int *ranks, int host_recv_bufs, route *routing_table);
    ~Proxy();
    void rdma_loop();

  private:
    const ProxyConfig config;
    rdma::server_cqs cqs;
    const unsigned num_clients;
    const int world_size;
    rdma::QP *local_qps;
    rdma::QP *remote_qps;
#ifdef LOCAL_DMA
    dma::Engine *dma_engine;
#endif
    std::atomic_bool quit;
    const size_t rx_depth;

    ibv_mr *h2d_mr;
    absl::flat_hash_map<unsigned, unsigned> qp_num_to_idx;
    int *idx_to_rank;

#ifndef LOCAL_DMA
    ibv_mr *d2h_mr;
#endif
    ReqBufWrite *d2h_bufs;
    int *client_recv_ready;
    std::atomic_bool *d2h_flushing;
    int host_recv_bufs;

    route *routing_table;

    void post_recv(uint64_t wr_id);

    void harvest_wcs();
    friend void *run_harvest_thread(void *arg);

    void route_reqs(char *buf, size_t len);
    bool try_flush_local();
};

} // namespace buddy::dpu
