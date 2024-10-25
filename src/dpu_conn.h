#pragma once

#include <mpi.h>
#include "rdma.h"
#include "dma.h"

namespace buddy::host {

class DpuConn {

  public:
    DpuConn(MPI_Comm comm, size_t h2d_size, size_t d2h_size);
    ~DpuConn();
    rdma::QP qp;
};

} // namespace buddy::host
