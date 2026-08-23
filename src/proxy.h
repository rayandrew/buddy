#pragma once

#include <atomic>
#include <list>
#include <vector>

#include "rdma.h"
#include "request.h"
#include "util.h"
#ifdef LOCAL_DMA
#include "dma.h"
#endif
#ifdef DOCA_FABRIC
#include "rdma_doca.h"
#endif

namespace buddy::dpu {

// D2D transport mode (DPU<->DPU leg). See BUDDY_D2D_MODE.
enum d2d_mode { D2D_SEND = 0, D2D_READ = 1, D2D_WRITE = 2 };

struct ProxyConfig {
  unsigned trace = 0;
  unsigned h2d_size = 0;
  unsigned d2h_size = 0;
  unsigned d2d_size = 32*1024;
  unsigned bufcount_remote = 2;
  unsigned bufcount_local = 2;
  double timeout = 60.0;
  double quiet_time = 1e-3;
  unsigned d2d_mode = D2D_SEND;   // 0 send (push), 1 read (pull), 2 write (push, one-sided)
};

inline std::ostream& operator<<(std::ostream& os, const ProxyConfig& config)
{
  os << "trace " << config.trace << std::endl;
  os << "h2d_size " << config.h2d_size << std::endl;
  os << "d2h_size " << config.d2h_size << std::endl;
  os << "d2d_size " << config.d2d_size << std::endl;
  os << "timeout " << config.timeout << std::endl;
  os << "quiet_time " << config.quiet_time << std::endl;
  os << "d2d_mode " << config.d2d_mode << std::endl;
  return os;
}

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
  uint16_t tid;
  uint16_t repid;

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

class SizeHistogram {
  public:
    SizeHistogram() = default;
    void record(size_t size);
    void add(const SizeHistogram& other);
    void reduce();
    void print();

  private:
    static const unsigned levels = 10;
    uint64_t counts[levels] = {};
};

class SendBufs {
  // Valid transitions:
  // READY -> FLUSHING by owner thread (the sender)
  // FLUSHING -> COMPLETE by any thread (the poller)
  // COMPLETE -> READY by owner thread (the sender)

  public:
    SendBufs() = default;
    SendBufs(unsigned n, unsigned m, unsigned size);
    ~SendBufs();

    ReqBufWrite &reqs(unsigned i, unsigned j) { return reqbufs[idx(i, j)]; }
    size_t offset(unsigned i, unsigned j) { return idx(i, j)*size; }
    ibv_mr *mr() { return mr_; }

    const SizeHistogram& get_hist() const { return size_hist; }

    // Called from owner thread
    bool ready_for_send(unsigned i, unsigned j)
    {
      if (is_ready[idx(i, j)])
        return true;

      if (is_complete[idx(i, j)]) {
        reqbufs[idx(i, j)].reset_pos();
        is_ready[idx(i, j)] = true;

        return true;
      }

      // Buffer is being flushed
      return false;
    }

    // Called from owner thread
    void mark_flushing(unsigned i, unsigned j)
    {
      assert(is_ready[idx(i, j)]);

      flush_count++;
      size_t size = reqbufs[idx(i, j)].get_pos();
      total_bytes += size;
      size_hist.record(size);

      is_ready[idx(i, j)] = false;
      is_complete[idx(i, j)] = false;
    }

    // Called from any thread
    void mark_complete(unsigned i, unsigned j)
    {
      assert(!is_complete[idx(i, j)]);
      is_complete[idx(i, j)] = true;
    }

    void get_counts(uint64_t *count, uint64_t *bytes)
    {
      *count = flush_count;
      *bytes = total_bytes;
    }

    // Called from owner thread
    int get_ready_repid(unsigned i)
    {
      unsigned start = repid_ptrs[i];

      for (unsigned k = 0; k < m; k++) {
        unsigned j = (start + k) % m;

        if (ready_for_send(i, j)) {
          repid_ptrs[i] = j;
          return j;
        }
      }

      return -1;
    }

  private:
    const unsigned n = 0;
    const unsigned m = 0;
    const unsigned size = 0;

    ibv_mr *mr_ = NULL;
    ReqBufWrite *reqbufs = NULL;
    bool *is_ready = NULL;
    std::atomic_bool *is_complete = NULL;
    unsigned *repid_ptrs = NULL;

    uint64_t flush_count = 0;
    uint64_t total_bytes = 0;
    SizeHistogram size_hist;

    unsigned idx(unsigned i, unsigned j) { return j + i*m; }
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
  // pull mode: a blocked read must ack its peer once drained (so the sender frees its buffer)
  bool needs_ack = false;
  unsigned ack_peer = 0;
  uint64_t ack_id = 0;
};

class Proxy {
  public:
    Proxy(ProxyConfig config, proxy_cqs cqs, unsigned num_clients,
        unsigned num_remotes, int world_size,
        rdma::QP *local_qps, rdma::QP *remote_qps,
#ifdef LOCAL_DMA
        dma::Engine *dma_engine,
#endif
#ifdef DOCA_FABRIC
        rdma::DocaRdma *doca_fabric, char *fabric_mem,
#endif
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
#ifdef LOCAL_DMA
    dma::Engine *dma_engine;           // proxy runs single-threaded under LOCAL_DMA (no lock)
    unsigned local_idx_of(uint32_t qp_num);
    void dma_xfer(unsigned client, uint32_t offset, uint32_t len, dma::direction dir);
    // windowed staging: SEND/RECV sliced into slots; the proxy owns the RECV (D2H) slot ring.
    unsigned dma_wsend, dma_wrecv;
    std::vector<uint32_t> dma_recv_free;
#endif
#ifdef DOCA_FABRIC
    // D2D over doca_rdma. Aggregated buffers are staged into fabric_mem (send half | recv half),
    // one send slot / recv slot per (remote, repid). Proxy is single-threaded (one doca_rdma pe).
    rdma::DocaRdma *doca_fabric;
    char *fabric_mem;
    size_t fabric_stage;               // bytes in send/recv half = fabric slots * d2d_size
    size_t fabric_ctrl;                // control region (READY/ACK descriptors) = 2*fabric_stage
    void fabric_poll(std::list<blocked_req>& blocked_reqs);
    void fabric_ack(unsigned peer, uint32_t slot, uint64_t id);
    void fabric_credit(unsigned peer, uint32_t slot);
#endif

    const int num_threads;
    const size_t h2d_depth;
    const size_t d2d_depth;
    const size_t rx_depth;
    const size_t local_recv_len;   // local recv buffer size: h2d_size, or a tiny ctrl slot under LOCAL_DMA
    const int poll_batch;          // completions drained per ibv_poll_cq (BUDDY_POLL_BATCH, default 1)

    ibv_mr *h2d_mr;
    ibv_mr *d2d_mr;
    int *local_idx_to_rank;

    // pull mode: one in-flight read per recv slot, so index by slot idx (no locking).
    struct pending_read { unsigned peer; uint64_t desc_id; uint32_t len; };
    pending_read *pending_reads = nullptr;

    // write mode: peers RDMA_WRITE into landing_mr; each peer owns a disjoint sub-region so
    // its slots never collide. peer_landing (from the MRINFO exchange) is where we write TO.
    ibv_mr *landing_mr = nullptr;
    struct peer_land { uint64_t addr; uint32_t rkey; uint32_t base_slot; uint32_t num_slots; };
    peer_land *peer_landing = nullptr;   // [num_remotes]
    unsigned slots_per_peer = 0;
    void d2d_write_exchange();

    SendBufs *d2h_send;
    SendBufs *d2d_send;
    double *last_thread_progress;

    route *routing_table;

    std::atomic_uint quit_counter;
    rdma_counters *in_counters = nullptr;

    void post_recv(route rt);
    char *get_recv_buf(route rt);

    bool poll_recv_queue(std::list<blocked_req>& blocked_reqs);
    void process_recv_wc(const ibv_wc& wc, std::list<blocked_req>& blocked_reqs);
    void poll_send_queue(std::list<blocked_req>& blocked_reqs);

    unsigned remote_idx_of(uint32_t qp_num);
    void handle_read_complete(uint32_t slot, std::list<blocked_req>& blocked_reqs);

    bool route_reqs(ReqBufRead& reader, std::list<blocked_req>& blocked_reqs);
    size_t flush_all();
    bool flush(route r, unsigned repid);
    bool flush_local(unsigned idx, unsigned repid);
    bool flush_remote(unsigned idx, unsigned repid);

    void print_counters();
};

} // namespace buddy::dpu
