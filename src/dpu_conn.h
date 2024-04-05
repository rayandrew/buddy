#pragma once

#include "rdma.h"
#include "dma.h"

namespace buddy::host {

class DpuConn {

  public:
    DpuConn(int world_rank, int world_size, dma::Buffer *dma_buf);
    ~DpuConn();
    rdma::QP qp;
};

} // namespace buddy::host
