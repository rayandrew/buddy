#pragma once

#include <cstddef>
#include <cstdint>

struct doca_dev;
struct doca_dpa;
struct doca_dpa_completion;
struct doca_rdma;
struct doca_ctx;
struct doca_rdma_connection;
struct rdma_event_channel;
struct rdma_cm_id;

namespace buddy::rdma {

// The DPU end of buddy's host leg, with its data path on the DPA so a message from the host lands in
// device memory and the Arm never sees it.
//
// The host side stays plain ibverbs. The two meet through RDMA CM, not doca_rdma_export: that blob
// is opaque to ibverbs, and the host runs a different DOCA version with no SDK headers at all.
// DOCA's bridge accepts an rdma_cm_id from an application that owns its own listen, which is what
// this does.
//
// Buffers are not this class's concern. The device posts receives with the routing engine's mmap
// handle over the same DPA pool, which is what puts a host message directly where the record walk
// reads it.
class HostLeg {
  public:
    HostLeg(doca_dpa *dpa, doca_dev *dev);
    ~HostLeg();

    // Share the routing engine's completion context, so a host-leg completion wakes the same thread
    // that routes the peer leg. Must be called before listen().
    void attach(doca_dpa_completion *comp);

    void listen(uint16_t port);

    // Returns false on timeout. Blocks otherwise until the host is connected. DOCA takes ownership
    // of the accepted cm_id.
    bool accept_one(double timeout_s = 60.0);

    // The handle the device kernel posts receives against. Valid after attach().
    uint64_t dpa_handle() const { return handle; }

  private:
    doca_dpa *dpa = nullptr;
    doca_dev *dev = nullptr;
    doca_rdma *rdma = nullptr;
    doca_ctx *ctx = nullptr;
    doca_rdma_connection *conn = nullptr;
    rdma_event_channel *ec = nullptr;
    rdma_cm_id *listen_id = nullptr;
    uint64_t handle = 0;
};

} // namespace buddy::rdma
