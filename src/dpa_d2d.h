#pragma once

#include <cstddef>
#include <cstdint>

struct doca_dev;
struct doca_dpa;
struct doca_mmap;
struct doca_rdma;
struct doca_ctx;
struct doca_dpa_completion;
struct doca_dpa_thread;

namespace buddy::rdma {

// DPA-native D2D engine: the device receives, routes and forwards, and the Arm is not in the data
// path. Buffers are DPA memory registered for RDMA, so the kernel reads them with a plain load and
// a forward is posted straight out of the buffer a message arrived in. See DESIGN-DPA.md.
//
// One engine drives one DPA thread over its own queue pair and its own slice of the receive pool,
// because the record walk is serial inside a buffer and independent across buffers.
class DpaD2D {
  public:
    struct stats {
        uint64_t wakes, tx_msgs, rx_msgs, records, forwards, local, bad_records, errors, last_err;
    };

    static constexpr unsigned kMaxEngines = 16;
    static constexpr unsigned kMaxRanks = 64;
    static constexpr uint8_t kLocal = 0xFF;

    DpaD2D(unsigned num_engines, unsigned bufs_per_engine, size_t buf_size);
    ~DpaD2D();

    // Both ends must call this for the peer in the same order, which is what pairs the engines.
    void connect(int sock, bool is_server);
    void wait_connected(double timeout_s = 60.0);

    // Ranks default to local; anything left local is counted and dropped rather than forwarded.
    void route_to_peer(int rank);

    // Posts every engine's receives and starts the threads. Nothing may be routed before this.
    void start();

    // Fills the transmit pool with @p records evenly sized records and starts @p window sends per
    // engine, so the device has a load to route without the Arm feeding it.
    void generate(unsigned window, unsigned records);

    stats sample() const;

  private:
    struct Engine;
    void init_engine(Engine &e, unsigned index);

    unsigned num_engines;
    unsigned bufs_per_engine;
    size_t buf_size;

    doca_dev *dev = nullptr;
    doca_dev *pf_dev = nullptr;
    doca_dpa *pf_dpa = nullptr;
    doca_dpa *dpa = nullptr;
    doca_mmap *mmap = nullptr;
    uint64_t pool_dev = 0;      // rx for every engine, then tx
    uint64_t pending_dev = 0;   // per-engine send FIFO, kept out of the argument block
    uint64_t args_dev = 0;
    Engine *engines = nullptr;
    uint8_t route[kMaxRanks];
};

} // namespace buddy::rdma
