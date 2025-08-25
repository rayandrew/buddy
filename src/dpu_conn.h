#pragma once

#include <stdint.h>
#include <mpi.h>
#include "rdma.h"
#include "dma.h"

namespace buddy::host {

class DpuConn {

  public:
    DpuConn(MPI_Comm comm, int32_t h2d_size, int32_t d2h_size);
    ~DpuConn();
    rdma::QP qp;

  private:
    MPI_Comm world_comm;
};

} // namespace buddy::host
