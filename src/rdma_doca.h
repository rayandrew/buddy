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

class DocaRdma {
  public:
    enum op_type { OP_SEND, OP_RECV, OP_READ, OP_WRITE };
    struct completion { uint64_t wr_id; uint32_t imm; uint32_t len; unsigned conn; op_type op; };

    struct Lane {
      DocaRdma *owner;
      unsigned index;
      doca_rdma *rdma;
      doca_ctx *ctx;
      doca_pe *pe;
      doca_buf_inventory *inv;
      doca_rdma_connection **conns;
      std::queue<completion> completed;
      unsigned established = 0;
      bool failed = false;
    };

    // mem = a single region covering both send and recv buffers (registered once).
    DocaRdma(unsigned num_connections, char *mem, size_t mem_len, unsigned num_lanes);
    ~DocaRdma();

    // connect one peer on every lane: export each lane's blob, swap over sock, connect. Both ends
    // must call this for the same peer in the same order, which is what pairs the lanes.
    void connect(unsigned idx, int sock, bool is_server);
    // pe_progress until every lane's ctx is RUNNING; aborts rather than hanging.
    void wait_connected(double timeout_s = 60.0);

    // @p lane selects the owning thread's queues; it must be the calling thread's own lane.
    void send_imm(unsigned lane, unsigned conn_idx, uint32_t imm, size_t offset, size_t len, uint64_t wr_id);
    void post_recv(unsigned lane, size_t offset, size_t len, uint64_t wr_id);
    // one-sided: read peer[conn] mem[remote_off] -> local mem[local_off]; write is the reverse.
    void read(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id);
    void write(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id);
    // write + immediate: peer gets an OP_RECV completion carrying imm (consumes a posted recv).
    void write_imm(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint32_t imm, uint64_t wr_id);
    bool poll(unsigned lane, completion *c);         // drives pe_progress; false if none ready
    // Arms the engine and sleeps until a completion arrives or timeout_s elapses. Only safe
    // when the caller has nothing else to service, and the timeout bounds how long any other
    // source waits. True if a completion is ready afterwards.
    bool wait_idle(unsigned lane, double timeout_s);

    unsigned lanes_count() const { return num_lanes; }
    unsigned conn_index_of(const Lane *l, const struct doca_rdma_connection *c);

  private:
    Lane &lane_of(unsigned lane) { return lanes[lane < num_lanes ? lane : 0]; }
    void init_lane(Lane &l, unsigned index);

    unsigned num_connections;
    unsigned num_lanes;
    char *mem;
    size_t mem_len;

    Lane *lanes;
    doca_dev *dev;                       // shared: registration, not queueing
    doca_mmap *mmap;
    doca_mmap **remote_mmap;             // peer memory imported for read/write
    char **remote_base;                  // peer mem base addr (for remote offsets)
};

} // namespace buddy::rdma
