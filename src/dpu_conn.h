#pragma once

#include "rdma.h"
#include "dma.h"

namespace buddy::host {

class DpuConn {

  public:
    DpuConn() = default;
    DpuConn(int world_rank, int world_size, dma::Buffer *dma_buf);
    ~DpuConn();

    rdma::QP qp = {};

  private:
    bool initialized = false;
};

} // namespace buddy::host
