#include <cassert>
#include "proxy.h"
#include "util.h"
#include "local_proto.h"
#include "valgrind/memcheck.h"

namespace buddy::dpu {

Proxy::~Proxy()
{
  char *h2d_buf = (char *)h2d_mr->addr;
  CHECK(!ibv_dereg_mr(h2d_mr));
  delete[] h2d_buf;

  delete[] d2h_bufs;
#ifndef LOCAL_DMA
  char *buf = (char *)d2h_mr->addr;
  CHECK(!ibv_dereg_mr(d2h_mr));
  delete[] buf;
#endif

  delete[] d2h_flushing;
  delete[] client_recv_ready;
}

Proxy::Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients, int world_size,
    rdma::QP *local_qps, rdma::QP *remote_qps,
#ifdef LOCAL_DMA
    dma::Engine *dma_engine,
#endif
    int *ranks, int host_recv_bufs, route *routing_table)
  : config(config)
  , cqs(cqs)
  , num_clients(num_clients)
  , world_size(world_size)
  , local_qps(local_qps)
  , remote_qps(remote_qps)
#ifdef LOCAL_DMA
  , dma_engine(dma_engine)
#endif
  , quit(false)
  , rx_depth(2*num_clients)
  , local_idx_to_rank(ranks)
  , host_recv_bufs(host_recv_bufs)
  , routing_table(routing_table)
{
  size_t total_size = PROXY_BUF_SIZE * rx_depth;
  char *h2d_buf = new char[total_size];

  h2d_mr = ibv_reg_mr(rdma::Context::get().get_pd(), h2d_buf, total_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  CHECK(h2d_mr);

  for (unsigned i = 0; i < num_clients; i++)
    qp_num_to_idx[local_qps[i].get_qp()->qp_num] = i;

  std::cout << "ranks:";
  for (unsigned i = 0; i < num_clients; i++)
    std::cout << " " << ranks[i];
  std::cout << std::endl;

  for (uint64_t wr_id = 0; wr_id < rx_depth; wr_id++) {
    post_recv(wr_id);
  }

  d2h_bufs = new ReqBufWrite[num_clients];
#ifdef LOCAL_DMA
  for (unsigned i = 0; i < num_clients; i++) {
    char *buf = dma_engine->client_buf(i) + DMA_OFFSET_RECV;
    new (&d2h_bufs[i]) ReqBufWrite(buf, DMA_SIZE_RECV);
  }
#else
  size_t d2h_size = DMA_SIZE_RECV * num_clients;
  char *bufs = new char[d2h_size];
  d2h_mr = ibv_reg_mr(rdma::Context::get().get_pd(), bufs, d2h_size, IBV_ACCESS_LOCAL_WRITE);
  CHECK(d2h_mr);

  for (unsigned i = 0; i < num_clients; i++)
    new (&d2h_bufs[i]) ReqBufWrite(bufs + i*DMA_SIZE_RECV, DMA_SIZE_RECV);

  d2h_flushing = new std::atomic_bool[num_clients];
  for (unsigned i = 0; i < num_clients; i++)
    d2h_flushing[i] = false;
#endif

  client_recv_ready = new int[num_clients];
  for (unsigned i = 0; i < num_clients; i++)
    client_recv_ready[i] = host_recv_bufs;
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
        FAIL("unsuccessful status " << wc[i].status << " (vendor_err " << wc[i].vendor_err << ")");

      if (wc[i].opcode & IBV_WC_RECV)
        FAIL("recv completion in send queue");

#ifndef LOCAL_DMA
      uint64_t wr_id = wc[i].wr_id;
      assert(d2h_flushing[wr_id]);
      d2h_bufs[wr_id].reset_pos();
      d2h_flushing[wr_id] = false;
#endif
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
  bool pending_d2h_flush = false;

  while (!quit) {
    ibv_wc wc[rx_depth];

    int n = ibv_poll_cq(cqs.recv, rx_depth, wc);
    CHECK(n >= 0);

    const double DEADLOCK_TIME = 1.0;
    double poll_start = 0.0;

    if (!n && pending_d2h_flush) {
      if (try_flush_local())
        pending_d2h_flush = false;
      else
        poll_start = clock();
    }

    while (n == 0 && !quit) {
      if (poll_start && clock()-poll_start > DEADLOCK_TIME) {
        std::cerr << "warning: Possible deadlock detected while waiting for client buffer to be ready!" << std::endl;
        poll_start = 0.0;
      }

      n = ibv_poll_cq(cqs.recv, rx_depth, wc);
    }
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
      char *recv_buf = (char *)h2d_mr->addr + wr_id * PROXY_BUF_SIZE;
      size_t msglen = wc[i].byte_len;
      VALGRIND_MAKE_MEM_DEFINED(recv_buf, msglen);

      switch (imm_tag) {
        case IMM_QUIT:
          {
            quit_counter++;
            if (quit_counter == num_clients)
              quit = true;
            break;
          }

        case IMM_H2D_DMA:
          {
#ifdef LOCAL_DMA
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
            local_qps[client_idx].write_imm(IMM_H2D_DMA);

            // Optimization: async
            char *src_buf = dma_engine->client_buf(client_idx) + DMA_OFFSET_SEND;
            route_reqs(src_buf, dmalen);

            pending_d2h_flush = true;
#else
            FAIL("unexpected dma message");
#endif
            break;
          }

        case IMM_D2H_DMA:
          {
#ifndef LOCAL_DMA
            FAIL("unexpected dma message");
#endif

            assert(!client_recv_ready[client_idx]);
            client_recv_ready[client_idx] = true;
            break;
          }

        case IMM_H2D_RDMA:
          {
            route_reqs(recv_buf, msglen);
            pending_d2h_flush = true;
            break;
          }

        case IMM_D2H_RDMA:
          {
            assert(client_recv_ready[client_idx] < host_recv_bufs);
            client_recv_ready[client_idx]++;
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
  uint64_t h2d_buf = (uint64_t) h2d_mr->addr;

  struct ibv_sge list = {
    .addr = h2d_buf + offset,
    .length = PROXY_BUF_SIZE,
    .lkey	= h2d_mr->lkey
  };
  VALGRIND_MAKE_MEM_UNDEFINED(list.addr, list.length);

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
    CHECK(head->dst >= 0 && head->dst < world_size);
    route r = routing_table[head->dst];
    if (r.remote) {
      std::cout << "route remote dpu " << r.idx << ", for rank " << head->dst << std::endl;
    } else {
      std::cout << "route local idx " << r.idx << ", for rank " << head->dst << std::endl;
      // Optimization: unnecessary to flush all buffers here
      // Also, could be async?
      while (!d2h_bufs[r.idx].append(*head, data))
        if (!try_flush_local())
          FAIL("recv buffer is full, but cannot be flushed!");
    }
  }
}

bool Proxy::try_flush_local()
{
  bool all_flushed = true;

  // Optimization: queue up all transfers at once
  for (unsigned i = 0; i < num_clients; i++) {
    if (d2h_bufs[i].empty() || d2h_flushing[i])
      continue;

    if (!client_recv_ready[i]) {
      all_flushed = false;
      continue;
    }

    --client_recv_ready[i];

    auto size = d2h_bufs[i].get_pos();
    CHECK((uint32_t)size == size);

#ifdef LOCAL_DMA
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

    local_qps[i].send_imm_inline(IMM_D2H_DMA, (char*)&size, sizeof(size));
    d2h_bufs[i].reset_pos();
#else
    local_qps[i].send_imm(IMM_D2H_RDMA, d2h_mr, size, i*DMA_SIZE_RECV, i);
    d2h_flushing[i] = true;
#endif
  }

  return all_flushed;
}

} // namespace buddy::dpu
