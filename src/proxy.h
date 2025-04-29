#pragma once

#include <atomic>
#include <list>

#include "rdma.h"
#include "request.h"
#include "util.h"

namespace buddy::dpu {

struct ProxyConfig {
  unsigned trace = 0;
  unsigned h2d_size = 0;
  unsigned d2h_size = 0;
  unsigned d2d_size = 64*1024*1024;
  double timeout = 5.0;
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

struct send_buf_id {
  route rt;
  uint32_t tid;

  inline uint64_t as_int()
  {
    uint64_t x = 0;
    static_assert(sizeof(x) == sizeof(*this));
    memcpy(&x, this, sizeof(x));
    return x;
  }

  static send_buf_id from_int(uint64_t u64)
  {
    send_buf_id id = {};
    static_assert(sizeof(id) == sizeof(u64));
    memcpy(&id, &u64, sizeof(id));
    return id;
  }
};

class SendBufs {
  public:
    SendBufs() : n(0), size(0) {}
    SendBufs(unsigned n, unsigned size);
    ~SendBufs();

    ReqBufWrite &reqs(unsigned i) { return reqbufs[i]; }
    size_t offset(unsigned i) { return i*size; }
    ibv_mr *mr() { return mr_; }
    bool is_flushing(unsigned i) { return flushing[i]; }
    void set_flushing(unsigned i, bool x)
    {
      assert(flushing[i] != x);

      if (x) {
        flush_count++;
        total_bytes += reqbufs[i].get_pos();
      }

      flushing[i] = x;
    }
    void get_counts(uint64_t *count, uint64_t *bytes)
    {
      *count = flush_count;
      *bytes = total_bytes;
    }

  private:
    const unsigned n;
    const unsigned size;

    ibv_mr *mr_;
    ReqBufWrite *reqbufs;
    std::atomic_bool *flushing;
    //bool *flushing;

    uint64_t flush_count;
    uint64_t total_bytes;
};

struct proxy_cqs {
  ibv_srq *local_srq;
  ibv_srq *remote_srq;
  ibv_cq *send;
  ibv_cq *local_recv;
  ibv_cq *remote_recv;
};

struct rdma_counters {
  uint64_t count_local = 0;
  uint64_t count_remote = 0;
  uint64_t bytes_local = 0;
  uint64_t bytes_remote = 0;
};

struct blocked_req {
  route recv_rt;
  ReqBufRead reqbuf;
  double deadline;
};

class Proxy {
  public:
    Proxy(ProxyConfig config, proxy_cqs cqs, unsigned num_clients,
        unsigned num_remotes, int world_size,
        rdma::QP *local_qps, rdma::QP *remote_qps,
        int *ranks, route *routing_table);
    ~Proxy();
    void rdma_loop();

  private:
    const ProxyConfig config;
    const unsigned num_clients;
    const unsigned num_remotes;
    const int world_size;

    proxy_cqs cqs;
    rdma::QP *local_qps;
    rdma::QP *remote_qps;

    const size_t h2d_depth;
    const size_t d2d_depth;
    const size_t rx_depth;

    ibv_mr *h2d_mr;
    ibv_mr *d2d_mr;
    int *local_idx_to_rank;

    SendBufs *d2h_send;
    SendBufs *d2d_send;

    route *routing_table;

    std::atomic_uint quit_counter;
    rdma_counters *in_counters = nullptr;

    void post_recv(route rt);
    char *get_recv_buf(route rt);

    bool poll_recv_queue(std::list<blocked_req>& blocked_reqs);
    void poll_send_queue();

    bool route_reqs(ReqBufRead& reader);
    void flush_all();
    void flush(route r);
    void flush_local(unsigned idx);
    void flush_remote(unsigned idx);

    void print_counters(int num_threads);
};

} // namespace buddy::dpu
