#pragma once

#include <mpi.h>
#include "rdma.h"
#include "dma.h"

namespace buddy::host {

class DpuConn {

  public:
    DpuConn(MPI_Comm comm);
    ~DpuConn();
    rdma::QP qp;
};

} // namespace buddy::host
