#include <cassert>
#include "proxy.h"
#include "util.h"
#include "local_proto.h"

namespace buddy::dpu {

Proxy::~Proxy()
{
  char *buffer = (char *)mr->addr;
  CHECK(!ibv_dereg_mr(mr));
  delete[] buffer;

  delete[] recv_bufs;
  delete[] client_recv_ready;
}

Proxy::Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients,
    rdma::QP *qps, dma::Engine *dma_engine, int *ranks)
  : config(config)
  , cqs(cqs)
  , num_clients(num_clients)
  , qps(qps)
  , dma_engine(dma_engine)
  , quit(false)
  , rx_depth(2*num_clients)
{
  size_t total_size = PROXY_BUF_SIZE * rx_depth;
  char *buffer = new char[total_size];

  mr = ibv_reg_mr(rdma::Context::get().get_pd(), buffer, total_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!mr) {
    perror("ibv_reg_mr");
    FAIL("failed to reg mr");
  }

  for (unsigned i = 0; i < num_clients; i++)
    qp_num_to_idx[qps[i].get_qp()->qp_num] = i;

  for (unsigned i = 0; i < num_clients; i++)
    rank_to_idx[ranks[i]] = i;
  delete[] ranks;

  for (uint64_t wr_id = 0; wr_id < rx_depth; wr_id++) {
    post_recv(wr_id);
  }

  recv_bufs = new ReqBufWrite[num_clients];
  for (unsigned i = 0; i < num_clients; i++) {
    char *buf = dma_engine->client_buf(i) + DMA_OFFSET_RECV;
    new (&recv_bufs[i]) ReqBufWrite(buf, DMA_SIZE_RECV);
  }

  client_recv_ready = new bool[num_clients];
  for (unsigned i = 0; i < num_clients; i++)
    client_recv_ready[i] = true;
}

// As this is not latency-critical, we could use completion events to save cpu
void Proxy::harvest_wcs()
{
  while (!quit) {
    ibv_wc wc[rx_depth];
    int n;

    do {
      n = ibv_poll_cq(cqs.send, rx_depth, wc);
    } while (n == 0 && !quit);

    if (n < 0) {
      perror("ibv_poll_cq");
      FAIL("ibv_poll_cq failed");
    }

    for (int i = 0; i < n; i++) {
      if (wc[i].status != IBV_WC_SUCCESS)
        FAIL("wc with error ");

      if (wc[i].opcode & IBV_WC_RECV)
        FAIL("recv completion in send queue");
    }
  }
}

void *run_harvest_thread(void *arg)
{
  Proxy *proxy = (Proxy *)arg;
  proxy->harvest_wcs();
  return NULL;
}

void Proxy::rdma_loop()
{
  pthread_t harvest_thread;

  uint64_t *hist_reqs = (uint64_t*)calloc(rx_depth, sizeof(uint64_t));

  if (pthread_create(&harvest_thread, NULL, run_harvest_thread, this) < 0)
    FAIL("failed to create thread");

  unsigned quit_counter = 0;

  while (!quit) {
    ibv_wc wc[rx_depth];

    int n;
    do {
      n = ibv_poll_cq(cqs.recv, rx_depth, wc);
    } while (n == 0 && !quit);

    CHECK(n >= 0);

    if (n > 0)
      hist_reqs[n-1]++;

    for (int i = 0; i < n; i++) {
      CHECK(wc[i].status == IBV_WC_SUCCESS);
      CHECK(wc[i].opcode == IBV_WC_RECV || wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM);
      CHECK(wc[i].wc_flags & IBV_WC_WITH_IMM);

      unsigned client_idx = qp_num_to_idx[wc[i].qp_num];
      uint32_t imm_tag = wc[i].imm_data;

      uint64_t wr_id = wc[i].wr_id;
      char *recv_buf = (char *)mr->addr + wr_id * PROXY_BUF_SIZE;
      size_t msglen = wc[i].byte_len;

      switch (imm_tag) {
        case IMM_QUIT:
          {
            quit_counter++;
            if (quit_counter == num_clients)
              quit = true;
            break;
          }
        case IMM_DMA_SEND_BUF:
          {
            CHECK(msglen == sizeof(uint64_t));

            uint64_t dmalen = *((uint64_t *)recv_buf);
            assert(dmalen > 0);
            CHECK((uint32_t)dmalen == dmalen);

            dma::jobspec job = {
              .client = client_idx,
              .offset = DMA_OFFSET_SEND,
              .len = (uint32_t)dmalen,
              .dir = dma::H2D,
            };
            dma_engine->transfer(job);

            // Optimization: do this in another thread
            job = {};
            while (!dma_engine->poll(&job));

            CHECK(job.client == client_idx);
            CHECK(job.offset == DMA_OFFSET_SEND);
            CHECK(job.dir == dma::H2D);

            // Ack
            qps[client_idx].write_imm(IMM_DMA_SEND_BUF);

            // Optimization: async
            char *src_buf = dma_engine->client_buf(client_idx) + DMA_OFFSET_SEND;
            route_reqs(src_buf, dmalen);
            flush_dma();

            break;
          }

        case IMM_DMA_RECV_BUF:
          {
            assert(!client_recv_ready[client_idx]);
            client_recv_ready[client_idx] = true;
            break;
          }

        default:
          FAIL("unknown imm_tag for recv " << imm_tag);
          break;
      }

      post_recv(wc[i].wr_id);
    }
  }

  printf("hist_reqs = [");
  for (uint64_t i = 0; i < rx_depth; i++) {
    printf("%lu,", hist_reqs[i]);
  }
  printf("]\n");

  if (pthread_join(harvest_thread, NULL))
    FAIL("join harvest thread failed");

  free(hist_reqs);
}

void Proxy::post_recv(uint64_t wr_id)
{
  assert(wr_id >= 0);
  assert(wr_id < rx_depth);

  uint64_t offset = wr_id * PROXY_BUF_SIZE;
  uint64_t buffer = (uint64_t) mr->addr;

  struct ibv_sge list = {
    .addr = buffer + offset,
    .length = PROXY_BUF_SIZE,
    .lkey	= mr->lkey
  };

  struct ibv_recv_wr *bad_wr;
  struct ibv_recv_wr wr = {
    .wr_id = wr_id,
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
  };

  if (ibv_post_srq_recv(cqs.srq, &wr, &bad_wr)) {
    perror("ibv_post_recv");
    FAIL("failed to post recv");
  }
}

void Proxy::route_reqs(char *buf, size_t len)
{
  ReqBufRead reader(buf, len);

  request_head *head;
  char *data;
  while (reader.next(&head, &data, -1)) {
    unsigned dst_client = rank_to_idx[head->dst];
    // Optimization: unnecessary to flush all buffers here
    // Also, could be async?
    while (!recv_bufs[dst_client].append(*head, data))
      flush_dma();
  }
}

void Proxy::flush_dma()
{
  // Optimization: queue up all transfers at once
  for (unsigned i = 0; i < num_clients; i++) {
    if (recv_bufs[i].empty())
      continue;

    // We need to handle it somehow...
    CHECK(client_recv_ready[i]);
    client_recv_ready[i] = false;

    auto size = recv_bufs[i].get_pos();
    CHECK((uint32_t)size == size);

    dma::jobspec job = {
      .client = i,
      .offset = DMA_OFFSET_RECV,
      .len = (uint32_t)size,
      .dir = dma::D2H,
    };
    dma_engine->transfer(job);

    job = {};
    while (!dma_engine->poll(&job));
    CHECK(job.client == i);
    CHECK(job.offset == DMA_OFFSET_RECV);
    CHECK(job.dir == dma::D2H);

    qps[i].send_imm_inline(IMM_DMA_RECV_BUF, (char*)&size, sizeof(size));
    recv_bufs[i].reset_pos();
  }
}

} // namespace buddy::dpu
