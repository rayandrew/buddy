#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

struct doca_dev;
struct doca_dpa;
struct doca_dpa_completion;
struct doca_mmap;
struct doca_rdma;
struct doca_ctx;
struct doca_rdma_connection;
struct rdma_event_channel;
struct rdma_cm_id;

namespace buddy::rdma {

// The DPU end of buddy's host leg, with its data path on the DPA so a message from the host lands
// in device memory and the Arm never sees it.
//
// The host side stays plain ibverbs. The two meet through RDMA CM, not doca_rdma_export: that blob
// is opaque to ibverbs and the host runs a different DOCA version with no SDK headers. DOCA's
// bridge takes an rdma_cm_id from an application that owns its own listen, which is what this does.
class HostLeg {
  public:
    // @p buf_dev is DPA memory shared with the routing engine, so a host message is routed without
    // being copied across to the Arm.
    HostLeg(doca_dpa *dpa, doca_dev *dev, uint64_t buf_dev, size_t buf_bytes);
    ~HostLeg();

    // Share the engine's completion context, so host-leg and peer-leg completions wake the same
    // thread and one kernel can route between the two.
    void attach(doca_dpa_completion *comp);

    void listen(uint16_t port);
    bool accept_one(double timeout_s = 60.0);

    // Handle the device kernel posts against.
    uint64_t dpa_handle() const { return handle; }
    uint32_t mmap_handle() const { return mmap_dpa; }

  private:
    doca_dpa *dpa = nullptr;
    doca_dev *dev = nullptr;
    doca_mmap *mmap = nullptr;
    doca_rdma *rdma = nullptr;
    doca_ctx *ctx = nullptr;
    doca_rdma_connection *conn = nullptr;
    rdma_event_channel *ec = nullptr;
    rdma_cm_id *listen_id = nullptr;
    uint64_t handle = 0;
    uint32_t mmap_dpa = 0;
};

} // namespace buddy::rdma
