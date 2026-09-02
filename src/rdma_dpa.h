#pragma once

#include <atomic>
#include <mutex>
#include <cstddef>
#include <cstdint>

struct doca_dev;
struct doca_dpa;
struct doca_mmap;
struct doca_rdma;
struct doca_rdma_connection;
struct doca_ctx;
struct doca_dpa_completion;
struct doca_dpa_notification_completion;
struct doca_dpa_thread;

namespace buddy::rdma {

// D2D transport whose data path runs on a DPA thread instead of on the proxy threads.
//
// Same surface as DocaRdma, and the same socket export-blob handshake, so the proxy does not care
// which is underneath. The difference is where the work happens: submits and completions cross
// through two rings in host memory, so the proxy issues a store instead of a doca_pe_progress and
// reads a load instead of polling the engine. Routing stays on the Arm.
//
// A DPA context owns one queue pair, so lanes_count() is 1 and every proxy thread shares one pair
// of rings. Both rings are therefore multi-producer and multi-consumer: threads claim a slot with
// an atomic index, so calls from any thread are safe on the same lane.
class DpaFabric {
  public:
    enum op_type { OP_SEND, OP_RECV, OP_READ, OP_WRITE };
    struct completion { uint64_t wr_id; uint32_t imm; uint32_t len; unsigned conn; op_type op; };

    // Both mirrored in rdma_dpa_dev.c; the definitions must agree field for field.
    // Padded to 64B: the DPA reaches these through a window with a 64B alignment restriction.
    struct submit_slot {
        uint64_t wr_id; uint64_t local_off; uint64_t remote_off;
        uint32_t len; uint32_t imm; uint32_t conn; uint32_t op; uint64_t seq; uint64_t pad[2];
    };
    struct ring_slot {
        uint64_t wr_id; uint32_t imm; uint32_t len; uint32_t conn; uint32_t op; uint64_t seq;
        uint64_t pad[4];
    };

    static constexpr unsigned kMaxConns = 8;
    static constexpr unsigned kWridFifo = 1024;
    static constexpr unsigned kLineSize = 64;

    // lanes is accepted and ignored, so the proxy constructs either fabric the same way. Ring depth
    // comes from BUDDY_DPA_RING and must exceed the in-flight ceiling bufcount_remote*remotes*threads.
    DpaFabric(unsigned num_connections, char *mem, size_t mem_len, unsigned lanes);
    ~DpaFabric();

    void connect(unsigned idx, int sock, bool is_server);
    // Also starts the DPA thread, which cannot run until every peer's mmap handle is known.
    void wait_connected(double timeout_s = 60.0);

    // lane is accepted for parity with DocaRdma; one DPA context drives one queue pair.
    void send_imm(unsigned lane, unsigned conn_idx, uint32_t imm, size_t offset, size_t len, uint64_t wr_id);
    void post_recv(unsigned lane, size_t offset, size_t len, uint64_t wr_id);
    void read(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id);
    void write(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id);
    void write_imm(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint32_t imm, uint64_t wr_id);
    bool poll(unsigned lane, completion *c);
    bool wait_idle(unsigned lane, double timeout_s);

    unsigned lanes_count() const { return 1; }

  private:
    // Returns the ring sequence of the slot, so a caller can wait for the DPA to consume it.
    uint64_t submit(uint32_t op, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len,
                    uint32_t imm, uint64_t wr_id);
    void start_kernel();

    unsigned num_connections;
    char *mem;
    size_t mem_len;
    bool debug = false;
    uint32_t max_msg = 0;

    doca_dev *dev = nullptr;
    // A DPA process lives on the PF, so the context is created there and extended onto the RDMA
    // scalable function. Creating it directly on the SF fails in FlexIO with a PRM process error.
    doca_dev *pf_dev = nullptr;
    doca_dpa *pf_dpa = nullptr;
    doca_dpa *dpa = nullptr;
    doca_rdma *rdma = nullptr;
    doca_ctx *ctx = nullptr;
    doca_mmap *mmap = nullptr;
    doca_rdma_connection **conns = nullptr;
    doca_mmap **remote_mmap = nullptr;

    doca_dpa_completion *dpa_comp = nullptr;
    // Exists only because a completion context requires one; it is never run.
    doca_dpa_thread *thread = nullptr;
    uint64_t dpa_rdma_handle = 0;

    char *sub_mem = nullptr;
    submit_slot *sub = nullptr;
    uint64_t *sub_consumed = nullptr;
    doca_mmap *sub_mmap = nullptr;
    std::atomic<uint64_t> sub_tail{0};

    char *ring_mem = nullptr;
    ring_slot *ring = nullptr;
    uint64_t *ring_consumed = nullptr;
    doca_mmap *ring_mmap = nullptr;
    unsigned ring_len;
    std::atomic<uint64_t> ring_head{0};

    uint64_t arg_dev = 0;
    uint64_t stop_dev = 0;
    uint64_t wrid_dev = 0;
    uint64_t rwrid_dev = 0;
    std::atomic<uint64_t> posted{0};
    std::atomic<uint64_t> completed{0};
};

} // namespace buddy::rdma
