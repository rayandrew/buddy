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
  bool trace = false;
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

  inline uint32_t as_int()
  {
    uint32_t x = 0;
    static_assert(sizeof(x) == sizeof(*this));
    memcpy(&x, this, sizeof(x));
    return x;
  }

  static route from_int(uint64_t u64)
  {
    uint32_t u32 = (uint32_t)u64;
    CHECK(u32 == u64);
    route r = {};
    static_assert(sizeof(r) == sizeof(u32));
    memcpy(&r, &u32, sizeof(r));
    return r;
  }
};

class SendBufs {
  public:
    SendBufs(unsigned n, unsigned size);
    ~SendBufs();

    inline ReqBufWrite &reqs(unsigned i) { return reqbufs[i]; }
    inline size_t offset(unsigned i) { return i*size; }
    inline ibv_mr *mr() { return mr_; }
    inline bool is_flushing(unsigned i) { return flushing[i]; }
    inline void set_flushing(unsigned i, bool x)
    {
      assert(flushing[i] != x);
      flushing[i] = x;
    }

  private:
    unsigned n;
    unsigned size;
    ibv_mr *mr_;
    ReqBufWrite *reqbufs;
    std::atomic_bool *flushing;
};

class Proxy {
  public:
    Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients,
        unsigned num_remotes, int world_size,
        rdma::QP *local_qps, rdma::QP *remote_qps,
#ifdef LOCAL_DMA
        dma::Engine *dma_engine,
#endif
        int *ranks, route *routing_table);
    ~Proxy();
    void rdma_loop();

  private:
    const ProxyConfig config;
    rdma::server_cqs cqs;
    const unsigned num_clients;
    const unsigned num_remotes;
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
    int *local_idx_to_rank;

#ifdef LOCAL_DMA
    int *client_recv_ready;
#else
    ibv_mr *d2h_mr;
#endif
    ReqBufWrite *d2h_reqs;
    std::atomic_bool *d2h_flushing;

    route *routing_table;
    SendBufs d2d_send;

    void post_recv(uint64_t wr_id);

    void harvest_wcs();
    friend void *run_harvest_thread(void *arg);

    void route_reqs(char *buf, size_t len);
    bool flush_all();
    bool flush_local(unsigned idx);
    bool flush_remote(unsigned idx);
};

} // namespace buddy::dpu
