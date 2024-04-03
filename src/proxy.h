#pragma once

#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <atomic>

#include "rdma.h"
#include "dma.h"

#define PROXY_RX_DEPTH 128
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
        rdma::QP *qps, dma::Engine *dma_engine);
    ~Proxy();
    void rdma_loop();

  private:
    const ProxyConfig config;
    rdma::server_cqs cqs;
    const unsigned num_clients;
    rdma::QP *qps;
    dma::Engine *dma_engine;

    ibv_mr *mr;
    std::unordered_map<unsigned, unsigned> qp_num_to_idx;
    std::atomic_bool quit;

    doca_ctx *doca_context;
    doca_workq *workq;
    doca_buf_inventory *buf_inv;
    doca_mmap *buf_mmap;
    doca_buf **recv_buf_doca;
    doca_buf **extra_buf_doca;

    // For async docaz
    std::mutex workq_mutex;
    std::condition_variable workq_cv;
    int workq_submitted = 0;

    void post_recv(uint64_t wr_id);

    void harvest_wcs();

    friend void *run_harvest_thread(void *arg);
};

} // namespace buddy::dpu
