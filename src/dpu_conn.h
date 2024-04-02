#pragma once

#include "rdma.h"

namespace buddy::host {

class DpuConn {

  public:
    DpuConn() = default;
    DpuConn(int world_rank, int world_size);
    ~DpuConn();

    rdma::QP qp = {};

  private:
    bool initialized = false;
};

} // namespace buddy::host
