#pragma once

#include "rdma.h"

namespace buddy::host {

class DpuConn {

  public:
    DpuConn();
    DpuConn(int world_rank, int world_size);
    ~DpuConn();

  private:
    bool initialized;
    rdma::QP qp;
};

} // namespace buddy::host
