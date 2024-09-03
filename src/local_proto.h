#pragma once

#include <cstdint>

namespace buddy {

const int LOCAL_PORT = 22838;
const int REMOTE_PORT = 22839;

struct local_init {
  int32_t world_rank;
  int32_t world_size;
  int32_t local_size;
  int32_t local_dma;
  int32_t host_recv_bufs;
  int32_t send_address_table;
};

const size_t DMA_SIZE_SEND = 64*1024*1024;
const size_t DMA_SIZE_RECV = 64*1024*1024;

const size_t DMA_OFFSET_SEND = 0;
const size_t DMA_OFFSET_RECV = DMA_SIZE_RECV;

const size_t DMA_SIZE_TOTAL = DMA_SIZE_SEND + DMA_SIZE_RECV;

const size_t RDMA_SIZE = 4096;

const size_t D2D_SIZE = 64*1024*1024;

enum rdma_imm {
  IMM_QUIT,
  IMM_H2D_DMA,
  IMM_D2H_DMA,
  IMM_H2D_RDMA,
  IMM_D2H_RDMA,
  IMM_D2D_RDMA,
};

} // namespace buddy
