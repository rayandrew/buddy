#include <cassert>
#include "proxy.h"
#include "util.h"
#include "local_proto.h"
#include "valgrind/memcheck.h"

#define TRACE(x) do{if (config.trace){std::clog << x << std::endl;}}while(0)

namespace buddy::dpu {

Proxy::~Proxy()
{
  char *h2d_buf = (char *)h2d_mr->addr;
  CHECK(!ibv_dereg_mr(h2d_mr));
  delete[] h2d_buf;

  delete[] d2h_reqs;

#ifdef LOCAL_DMA
  delete[] client_recv_ready;
#else
  char *buf = (char *)d2h_mr->addr;
  CHECK(!ibv_dereg_mr(d2h_mr));
  delete[] buf;
#endif

  delete[] d2h_flushing;
}

Proxy::Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients,
    unsigned num_remotes, int world_size,
    rdma::QP *local_qps, rdma::QP *remote_qps,
#ifdef LOCAL_DMA
    dma::Engine *dma_engine,
#endif
    int *ranks, route *routing_table)
  : config(config)
  , cqs(cqs)
  , num_clients(num_clients)
  , num_remotes(num_remotes)
  , world_size(world_size)
  , local_qps(local_qps)
  , remote_qps(remote_qps)
#ifdef LOCAL_DMA
  , dma_engine(dma_engine)
#endif
  , quit(false)
  , rx_depth(2*num_clients)
  , local_idx_to_rank(ranks)
  , routing_table(routing_table)
  , d2d_send(num_remotes, D2D_SIZE)
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

  d2h_reqs = new ReqBufWrite[num_clients];
#ifdef LOCAL_DMA
  for (unsigned i = 0; i < num_clients; i++) {
    char *buf = dma_engine->client_buf(i) + DMA_OFFSET_RECV;
    new (&d2h_reqs[i]) ReqBufWrite(buf, DMA_SIZE_RECV);
  }
#else
  size_t d2h_size = DMA_SIZE_RECV * num_clients;
  char *bufs = new char[d2h_size];
  d2h_mr = ibv_reg_mr(rdma::Context::get().get_pd(), bufs, d2h_size, IBV_ACCESS_LOCAL_WRITE);
  CHECK(d2h_mr);

  for (unsigned i = 0; i < num_clients; i++)
    new (&d2h_reqs[i]) ReqBufWrite(bufs + i*DMA_SIZE_RECV, DMA_SIZE_RECV);

  d2h_flushing = new std::atomic_bool[num_clients];
  for (unsigned i = 0; i < num_clients; i++)
    d2h_flushing[i] = false;
#endif

#ifdef LOCAL_DMA
  client_recv_ready = new int[num_clients];
  for (unsigned i = 0; i < num_clients; i++)
    client_recv_ready[i] = host_recv_bufs;
#endif

  TRACE("trace on");
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
      if (wc[i].status != IBV_WC_SUCCESS) {
        std::cerr << "wc status " << wc[i].status << ": " << ibv_wc_status_str(wc[i].status) << std::endl;
        std::cerr << "vendor_err " << wc[i].vendor_err << std::endl;

        for (unsigned idx = 0; idx < num_clients; idx++)
          if (local_qps[idx].get_qp()->qp_num == wc[i].qp_num)
            std::cerr << "destination: rank " << local_idx_to_rank[idx] << " (local idx " << idx << ")" << std::endl;

        for (unsigned idx = 0; idx < num_remotes; idx++)
          if (remote_qps[idx].get_qp()->qp_num == wc[i].qp_num)
            std::cerr << "destination: remote dpu " << idx << std::endl;

        if (wc[i].status == IBV_WC_RNR_RETRY_EXC_ERR)
          std::cerr << "Maybe the host ran out of receive buffers, try increasing BUDDY_RECV_BUFS." << std::endl;
        FAIL("wc error");
      }

      if (wc[i].opcode & IBV_WC_RECV)
        FAIL("recv completion in send queue");

#ifndef LOCAL_DMA
      uint64_t wr_id = wc[i].wr_id;
      route rt = route::from_int(wr_id);
      if (rt.remote) {
        assert(d2d_send.is_flushing(rt.idx));
        d2d_send.reqs(rt.idx).reset_pos();
        d2d_send.set_flushing(rt.idx, false);
      } else {
        assert(d2h_flushing[rt.idx]);
        d2h_reqs[rt.idx].reset_pos();
        d2h_flushing[rt.idx] = false;
      }
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
  bool pending_flush = false;

  while (!quit) {
    ibv_wc wc[rx_depth];

    int n = ibv_poll_cq(cqs.recv, rx_depth, wc);
    CHECK(n >= 0);

    const double DEADLOCK_TIME = 1.0;
    double poll_start = 0.0;

    if (!n && pending_flush) {
      if (flush_all())
        pending_flush = false;
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

#ifdef LOCAL_DMA
      unsigned client_idx = qp_num_to_idx[wc[i].qp_num];
#endif
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

            pending_flush = true;
#else
            FAIL("unexpected dma message");
#endif
            break;
          }

        case IMM_D2H_DMA:
          {
#ifdef LOCAL_DMA
            assert(!client_recv_ready[client_idx]);
            client_recv_ready[client_idx] = true;
#else
            FAIL("unexpected dma message");
#endif
            break;
          }

        case IMM_H2D_RDMA:
          {
            TRACE("recv H2D_RDMA size " << msglen);
            route_reqs(recv_buf, msglen);
            pending_flush = true;
            break;
          }

        case IMM_D2D_RDMA:
          {
            TRACE("recv D2D_RDMA size " << msglen);
            route_reqs(recv_buf, msglen);
            pending_flush = true;
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
      TRACE("route to " << head->dst << " (remote " << r.idx << ")");
      while (d2d_send.is_flushing(r.idx));
      while (!d2d_send.reqs(r.idx).append(*head, data))
        if (!flush_remote(r.idx))
          FAIL("remote send buf is full but cannot be flushed!");
    } else {
      TRACE("route to " << head->dst << " (local " << r.idx << ")");
      // Optimization: unnecessary to flush all buffers here
      // Also, could be async?
      while (d2h_flushing[r.idx]);
      while (!d2h_reqs[r.idx].append(*head, data))
        if (!flush_local(r.idx))
          FAIL("recv buffer is full, but cannot be flushed!");
    }
  }
}

bool Proxy::flush_remote(unsigned idx)
{
  if (d2d_send.reqs(idx).empty() || d2d_send.is_flushing(idx))
    return true;

  d2d_send.set_flushing(idx, true);

  size_t size = d2d_send.reqs(idx).get_pos();
  TRACE("send to remote " << idx << " size " << size);
  remote_qps[idx].send_imm(IMM_D2D_RDMA, d2d_send.mr(),
      size, d2d_send.offset(idx),
      route::make_remote(idx).as_int());

  return true;
}

bool Proxy::flush_all()
{
  bool all_flushed = true;

  // Optimization: queue up all transfers at once
  for (unsigned i = 0; i < num_clients; i++)
    if (!flush_local(i))
      all_flushed = false;

  for (unsigned i = 0; i < num_remotes; i++)
    if (!flush_remote(i))
      all_flushed = false;

  return all_flushed;
}

bool Proxy::flush_local(unsigned idx)
{
  if (d2h_reqs[idx].empty() || d2h_flushing[idx])
    return true;;

  auto size = d2h_reqs[idx].get_pos();
  CHECK((uint32_t)size == size);

#ifdef LOCAL_DMA
  if (!client_recv_ready[idx])
    return false;

  --client_recv_ready[idx];

  dma::jobspec job = {
    .client = idx,
    .offset = DMA_OFFSET_RECV,
    .len = (uint32_t)size,
    .dir = dma::D2H,
  };
  dma_engine->transfer(job);

  job = {};
  while (!dma_engine->poll(&job));
  CHECK(job.client == idx);
  CHECK(job.offset == DMA_OFFSET_RECV);
  CHECK(job.dir == dma::D2H);

  local_qps[idx].send_imm_inline(IMM_D2H_DMA, (char*)&size, sizeof(size));
  d2h_reqs[idx].reset_pos();
#else
  assert(!d2h_flushing[idx]);
  d2h_flushing[idx] = true;
  TRACE("send to local rank " << local_idx_to_rank[idx] << " size " << size);
  local_qps[idx].send_imm(IMM_D2H_RDMA, d2h_mr, size, idx*DMA_SIZE_RECV, route::make_local(idx).as_int());
#endif

  return true;
}

SendBufs::SendBufs(unsigned n, unsigned size)
  : n(n)
  , size(size)
{
  reqbufs = new ReqBufWrite[n];
  char *buf = new char[n*size];
  mr_ = ibv_reg_mr(rdma::Context::get().get_pd(), buf, n*size, IBV_ACCESS_LOCAL_WRITE);
  CHECK(mr_);

  for (unsigned i = 0; i < n; i++)
    new (&reqbufs[i]) ReqBufWrite(buf + offset(i), size);

  flushing = new std::atomic_bool[n];
  for (unsigned i = 0; i < n; i++)
    flushing[i] = false;
}

SendBufs::~SendBufs()
{
  delete[] flushing;
  delete[] reqbufs;
  char *buf = (char *)mr_->addr;
  CHECK(!ibv_dereg_mr(mr_));
  delete[] buf;
}

} // namespace buddy::dpu
