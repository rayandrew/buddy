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
  IMM_D2D_MRINFO,  // write: receiver advertises its landing sub-region (payload = d2d_desc)
  IMM_D2D_WRITE,   // write: one-sided data; imm = (abs_slot << 8) | IMM_D2D_WRITE
  IMM_D2D_CREDIT,  // write: receiver frees a landing slot (payload = uint32 abs_slot)
  IMM_H2D_ACK,     // local DMA: proxy drained the host SEND slot (imm = slot<<8 | tag)
  IMM_D2H_CREDIT,  // local DMA: host copied the RECV slot out, proxy may reuse it (imm = slot<<8)
};
// imm carries the tag in the low byte; slot-carrying tags pack the slot in the high bits.
static const uint32_t IMM_TAG_MASK = 0xFF;
static const unsigned IMM_SLOT_SHIFT = 8;

// local DMA windowed staging: the 64 MB SEND/RECV regions are sliced into fixed-size slots
// (slot = g_max_send / g_min_recv), capped so slot indices fit the imm high bits and the
// control recv pools stay small. A slot is busy from producer stage until consumer credit.
static const unsigned DMA_MAX_SLOTS = 256;

// pull: reader RDMA_READs len bytes from (addr,rkey), then acks id to free the sender's buffer.
struct d2d_desc {
  uint64_t addr;
  uint64_t id;     // sender's send_buf_id (echoed back in the ack)
  uint32_t rkey;
  uint32_t len;
};

} // namespace buddy
