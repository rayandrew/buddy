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
  int32_t send_address_table;
  int32_t h2d_size;
  int32_t d2h_size;
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
  IMM_D2D_READY,   // pull: sender advertises a ready buffer (payload = d2d_desc)
  IMM_D2D_ACK,     // pull: reader signals read done (payload = d2d_desc.id)
};

// pull: reader RDMA_READs len bytes from (addr,rkey), then acks id to free the sender's buffer.
struct d2d_desc {
  uint64_t addr;
  uint64_t id;     // sender's send_buf_id (echoed back in the ack)
  uint32_t rkey;
  uint32_t len;
};

} // namespace buddy
