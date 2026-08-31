#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
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
    enum op_type { OP_SEND, OP_RECV, OP_READ, OP_WRITE };
    struct completion { uint64_t wr_id; uint32_t imm; uint32_t len; unsigned conn; op_type op; };

    // mem = a single region covering both send and recv buffers (registered once).
    DocaRdma(unsigned num_connections, char *mem, size_t mem_len);
    ~DocaRdma();

    // connect one peer: export our blob, swap over sock, connect; call for each peer.
    void connect(unsigned idx, int sock, bool is_server);
    // pe_progress until ctx RUNNING; aborts rather than hanging if that never happens.
    void wait_connected(double timeout_s = 60.0);

    void send_imm(unsigned conn_idx, uint32_t imm, size_t offset, size_t len, uint64_t wr_id);
    void post_recv(size_t offset, size_t len, uint64_t wr_id);
    // one-sided: read peer[conn] mem[remote_off] -> local mem[local_off]; write is the reverse.
    void read(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id);
    void write(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id);
    // write + immediate: peer gets an OP_RECV completion carrying imm (consumes a posted recv).
    void write_imm(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint32_t imm, uint64_t wr_id);
    bool poll(completion *c);                        // drives pe_progress; false if none ready
    // Arms the engine and sleeps until a completion arrives or timeout_s elapses. Only safe
    // when the caller has nothing else to service, and the timeout bounds how long any other
    // source waits. True if a completion is ready afterwards.
    bool wait_idle(double timeout_s);

    void push(const completion &c) { completed.push(c); }   // used by callbacks
    void on_established() { established++; }                 // used by callbacks
    void on_failed() { failed = true; }                      // used by callbacks
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
    doca_mmap **remote_mmap;             // peer memory imported for read/write
    char **remote_base;                  // peer mem base addr (for remote offsets)
    std::queue<completion> completed;
    // One progress engine shared by every proxy thread, as the ibverbs leg shares one CQ. Held
    // across submit and progress; completion callbacks run inside progress and must not retake it.
    std::mutex mu;
    unsigned established = 0;
    bool failed = false;
};

} // namespace buddy::rdma
