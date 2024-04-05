#pragma once

#include <cstdint>

namespace buddy {

const int LOCAL_PORT = 22838;

struct local_init {
  int32_t world_rank;
  int32_t world_size;
};

const size_t DMA_SIZE_SEND = 64*1024;
const size_t DMA_SIZE_RECV = 64*1024;

const size_t DMA_OFFSET_SEND = 0;
const size_t DMA_OFFSET_RECV = DMA_SIZE_RECV;

const size_t DMA_SIZE_TOTAL = DMA_SIZE_SEND + DMA_SIZE_RECV;

const size_t RDMA_SIZE = 4096;

enum rdma_imm {
  IMM_QUIT,
  IMM_DMA_SEND_BUF,
  IMM_DMA_RECV_BUF,
};

} // namespace buddy
