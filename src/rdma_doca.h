#pragma once

#include <cstdint>
#include <cstddef>
#include <queue>

struct doca_dev;
struct doca_rdma;
struct doca_ctx;
struct doca_pe;
struct doca_mmap;
struct doca_buf_inventory;
struct doca_rdma_connection;

namespace buddy::rdma {

// doca_rdma D2D transport (SEND mode) parallel to rdma::QP, for the all-DOCA fabric leg.
// One ctx/pe with N connections (peers). The caller supplies the send+recv memory; this
// registers it as a doca_mmap. send_imm/post_recv/poll mirror the ibverbs D2D usage.
// RoCE: BUDDY_RDMA_DEV (default mlx5_2, the SF device with GIDs) + gid index 0 + GRH.
class DocaRdma {
  public:
    struct completion { uint64_t wr_id; uint32_t imm; uint32_t len; unsigned conn; bool is_recv; };

    // mem = a single region covering both send and recv buffers (registered once).
    DocaRdma(unsigned num_connections, char *mem, size_t mem_len);
    ~DocaRdma();

    // connect one peer: export our blob, swap over sock, connect; call for each peer.
    void connect(unsigned idx, int sock, bool is_server);
    void wait_connected();                          // pe_progress until ctx RUNNING

    void send_imm(unsigned conn_idx, uint32_t imm, size_t offset, size_t len, uint64_t wr_id);
    void post_recv(size_t offset, size_t len, uint64_t wr_id);
    bool poll(completion *c);                        // drives pe_progress; false if none ready

    void push(const completion &c) { completed.push(c); }   // used by callbacks
    unsigned conn_index_of(const struct doca_rdma_connection *c);

  private:
    unsigned num_connections;
    char *mem;
    size_t mem_len;

    doca_dev *dev;
    doca_rdma *rdma;
    doca_ctx *ctx;
    doca_pe *pe;
    doca_mmap *mmap;
    doca_buf_inventory *inv;
    doca_rdma_connection **conns;
    std::queue<completion> completed;
};

} // namespace buddy::rdma
