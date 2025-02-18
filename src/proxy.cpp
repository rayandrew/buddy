#include <cassert>
#include <omp.h>
#include "proxy.h"
#include "util.h"
#include "local_proto.h"
#include "valgrind/memcheck.h"

#ifdef NDEBUG
#define TRACE(x) do{}while(0)
#else
#define TRACE(x) do{if (config.trace){std::clog << x << std::endl;}}while(0)
#endif

enum tt_clock {
  TT_RDMALOOP,
  TT_POLL,
  TT_ROUTE,
  TT_LOCFLUSH,
  TT_REMFLUSH,
  TT_D2DBLOCK,
  TT_D2HBLOCK,
  TT_APPBLOCK,
  TT_COUNT,
};

__attribute__((unused))
static const char *tt_label[TT_COUNT] = {
  "rdmaloop",
  "poll",
  "route",
  "locflush",
  "remflush",
  "d2dblock",
  "d2hblock",
  "appblock",
};

#include "tictoc.h"

namespace buddy::dpu {

Proxy::~Proxy()
{
  char *h2d_buf = (char *)h2d_mr->addr;
  CHECK(!ibv_dereg_mr(h2d_mr));
  delete[] h2d_buf;
}

Proxy::Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients,
    unsigned num_remotes, int world_size,
    rdma::QP *local_qps, rdma::QP *remote_qps,
    int *ranks, route *routing_table)
  : config(config)
  , cqs(cqs)
  , num_clients(num_clients)
  , num_remotes(num_remotes)
  , world_size(world_size)
  , local_qps(local_qps)
  , remote_qps(remote_qps)
  , rx_depth(4*num_clients)
  , local_idx_to_rank(ranks)
  , routing_table(routing_table)
{
#pragma omp parallel
  {
    int tid = omp_get_thread_num();
    int num_threads = omp_get_num_threads();

#pragma omp single
    {
      d2h_send = new SendBufs[num_threads];
      d2d_send = new SendBufs[num_threads];
    }

    // TODO: allow different d2d buffer size
    new (&d2h_send[tid]) SendBufs(num_clients, config.d2h_size);
    new (&d2d_send[tid]) SendBufs(num_remotes, config.h2d_size);
  }

  size_t total_size = config.h2d_size * rx_depth;
  CHECK(total_size);
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

  TRACE("trace on");
}

void Proxy::poll_send_queue()
{
  int num_threads = omp_get_num_threads();
  int thread_depth = rx_depth / num_threads;
  ibv_wc wc[thread_depth];

  int n = ibv_poll_cq(cqs.send, thread_depth, wc);

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

    uint64_t wr_id = wc[i].wr_id;
    auto id = send_buf_id::from_int(wr_id);

    auto send_bufs = &d2h_send[id.tid];
    if (id.rt.remote)
      send_bufs = &d2d_send[id.tid];

    assert(send_bufs->is_flushing(id.rt.idx));
    send_bufs->reqs(id.rt.idx).reset_pos();
    send_bufs->set_flushing(id.rt.idx, false);
  }
}

void Proxy::rdma_loop()
{
  auto *hist_reqs = new std::atomic_uint64_t[rx_depth];
  for (unsigned i = 0; i < rx_depth; i++)
    hist_reqs[i] = 0;

  std::atomic_uint quit_counter(0);

  uint64_t count_in_local = 0;
  uint64_t count_in_remote = 0;
  uint64_t count_out_local = 0;
  uint64_t count_out_remote = 0;

  uint64_t bytes_in_local = 0;
  uint64_t bytes_in_remote = 0;
  uint64_t bytes_out_local = 0;
  uint64_t bytes_out_remote = 0;

#pragma omp parallel reduction(+:count_in_local,count_in_remote,count_out_local,count_out_remote,bytes_in_local,bytes_in_remote,bytes_out_local,bytes_out_remote)
  {
    int num_threads = omp_get_num_threads();
    int thread_depth = rx_depth / num_threads;

    bool pending_flush = false;

#pragma omp barrier
#pragma omp single
    std::cout << num_threads << " rdma threads ready" << std::endl;

    tic(TT_RDMALOOP);

    while (quit_counter != num_clients) {
      ibv_wc wc[thread_depth];

      tic(TT_POLL);

      int n = ibv_poll_cq(cqs.recv, thread_depth, wc);
      CHECK(n >= 0);

      if (!n && pending_flush) {
        flush_all();
        pending_flush = false;
      }

      while (n == 0 && quit_counter != num_clients) {
        n = ibv_poll_cq(cqs.recv, thread_depth, wc);
      }
      CHECK(n >= 0);

      toc(TT_POLL);

      if (n > 0)
        hist_reqs[n-1]++;

      for (int i = 0; i < n; i++) {
        CHECK(wc[i].status == IBV_WC_SUCCESS);
        CHECK(wc[i].opcode == IBV_WC_RECV || wc[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM);
        CHECK(wc[i].wc_flags & IBV_WC_WITH_IMM);

        uint32_t imm_tag = wc[i].imm_data;

        uint64_t wr_id = wc[i].wr_id;
        char *recv_buf = (char *)h2d_mr->addr + wr_id * config.h2d_size;
        size_t msglen = wc[i].byte_len;
        VALGRIND_MAKE_MEM_DEFINED(recv_buf, msglen);

        switch (imm_tag) {
          case IMM_QUIT:
            {
              quit_counter++;
              break;
            }

          case IMM_H2D_RDMA:
            {
              TRACE("recv H2D_RDMA size " << msglen);

              count_in_local++;
              bytes_in_local += msglen;

              route_reqs(recv_buf, msglen);
              pending_flush = true;
              break;
            }

          case IMM_D2D_RDMA:
            {
              TRACE("recv D2D_RDMA size " << msglen);

              count_in_remote++;
              bytes_in_remote += msglen;

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

      poll_send_queue();
    }

    int tid = omp_get_thread_num();
    d2h_send[tid].get_counts(&count_out_local, &bytes_out_local);
    d2d_send[tid].get_counts(&count_out_remote, &bytes_out_remote);

    toc(TT_RDMALOOP);
  }

  uint64_t nnz = rx_depth;
  while (nnz && !hist_reqs[nnz-1])
    nnz--;

  printf("hist_reqs = [");
  for (uint64_t i = 0; i < nnz; i++) {
    printf("%lu,", hist_reqs[i].load());
  }
  printf("]\n");

  delete[] hist_reqs;

  std::cout << "--- msg counts ---" << std::endl;
  std::cout << "count_in_local\t" << count_in_local << std::endl;
  std::cout << "count_in_remote\t" << count_in_remote << std::endl;
  std::cout << "count_out_local\t" << count_out_local << std::endl;
  std::cout << "count_out_remote\t" << count_out_remote << std::endl;
  std::cout << "------------------" << std::endl;

  std::cout << "--- network bytes ---" << std::endl;
  std::cout << "bytes_in_local\t" << bytes_in_local << std::endl;
  std::cout << "bytes_in_remote\t" << bytes_in_remote << std::endl;
  std::cout << "bytes_out_local\t" << bytes_out_local << std::endl;
  std::cout << "bytes_out_remote\t" << bytes_out_remote << std::endl;
  std::cout << "---------------------" << std::endl;

  std::cout << "--- avg msg size ---" << std::endl;
  if (count_in_local)
    std::cout << "avg_in_local\t" << bytes_in_local/count_in_local << std::endl;
  if (count_in_remote)
    std::cout << "avg_in_remote\t" << bytes_in_remote/count_in_remote << std::endl;
  if (count_out_local)
    std::cout << "avg_out_local\t" << bytes_out_local/count_out_local << std::endl;
  if (count_out_remote)
    std::cout << "avg_out_remote\t" << bytes_out_remote/count_out_remote << std::endl;
  std::cout << "--------------------" << std::endl;

  tt_print("proxy breakdown");
}

void Proxy::post_recv(uint64_t wr_id)
{
  assert(wr_id >= 0);
  assert(wr_id < rx_depth);

  uint64_t offset = wr_id * config.h2d_size;
  uint64_t h2d_buf = (uint64_t) h2d_mr->addr;

  struct ibv_sge list = {
    .addr = h2d_buf + offset,
    .length = (uint32_t)config.h2d_size,
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
  tic(TT_ROUTE);

  int tid = omp_get_thread_num();
  ReqBufRead reader(buf, len);

  request_head *head;
  char *data;
  while (reader.next(&head, &data)) {
    CHECK(head->dst >= 0 && head->dst < world_size);
    route r = routing_table[head->dst];

    auto clock = TT_D2HBLOCK;
    auto send_bufs = &d2h_send[tid];
    if (r.remote) {
      clock = TT_D2DBLOCK;
      send_bufs = &d2d_send[tid];
      TRACE("route to " << head->dst << " (remote " << r.idx << ")");
    } else {
      TRACE("route to " << head->dst << " (local " << r.idx << ")");
    }

    if (send_bufs->is_flushing(r.idx)) {
      tic(clock);

      double t0 = omp_get_wtime();
      for (;;) {
        poll_send_queue();
        if (!send_bufs->is_flushing(r.idx))
          break;

        if (config.timeout >= 0 && omp_get_wtime() - t0 > config.timeout)
          FAIL("Timed out waiting for flush to complete");
      }

      toc(clock);
    }

    ReqBufWrite& req = send_bufs->reqs(r.idx);
    if (!req.append(*head, data)) {
      tic(TT_APPBLOCK);

      double t0 = omp_get_wtime();
      do {
        if (send_bufs->is_flushing(r.idx))
          poll_send_queue();
        else
          flush(r);

        if (config.timeout >= 0 && omp_get_wtime() - t0 > config.timeout)
          FAIL("Timed out waiting to append send buffer");
      } while (!req.append(*head, data));

      toc(TT_APPBLOCK);
    }
  }

  toc(TT_ROUTE);
}

void Proxy::flush(route r)
{
  if (r.remote)
    flush_remote(r.idx);
  else
    flush_local(r.idx);
}

void Proxy::flush_remote(unsigned idx)
{
  int tid = omp_get_thread_num();

  assert(!d2d_send[tid].is_flushing(idx));

  if (d2d_send[tid].reqs(idx).empty())
    return;

  tic(TT_REMFLUSH);

  d2d_send[tid].set_flushing(idx, true);

  size_t size = d2d_send[tid].reqs(idx).get_pos();
  route rt = route::make_remote(idx);
  send_buf_id id = {rt, (uint32_t)tid};

  TRACE("send to remote " << idx << " size " << size << " thread " << tid);
  remote_qps[idx].send_imm(IMM_D2D_RDMA, d2d_send[tid].mr(),
      size, d2d_send[tid].offset(idx), id.as_int());

  toc(TT_REMFLUSH);
}

void Proxy::flush_local(unsigned idx)
{
  int tid = omp_get_thread_num();

  assert(!d2h_send[tid].is_flushing(idx));

  if (d2h_send[tid].reqs(idx).empty())
    return;

  tic(TT_LOCFLUSH);

  d2h_send[tid].set_flushing(idx, true);

  size_t size = d2h_send[tid].reqs(idx).get_pos();
  route rt = route::make_local(idx);
  send_buf_id id = {rt, (uint32_t)tid};

  TRACE("send to local rank " << local_idx_to_rank[idx] << " size " << size << " thread " << tid);
  local_qps[idx].send_imm(IMM_D2H_RDMA, d2h_send[tid].mr(), size,
      d2h_send[tid].offset(idx), id.as_int());

  toc(TT_LOCFLUSH);
}

void Proxy::flush_all()
{
  int tid = omp_get_thread_num();

  for (unsigned i = 0; i < num_clients; i++)
    if (!d2h_send[tid].is_flushing(i))
      flush_local(i);

  for (unsigned i = 0; i < num_remotes; i++)
    if (!d2d_send[tid].is_flushing(i))
      flush_remote(i);
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
  //flushing = new bool[n];
  for (unsigned i = 0; i < n; i++)
    flushing[i] = false;
}

SendBufs::~SendBufs()
{
  if (flushing)
    delete[] flushing;
  if (reqbufs)
    delete[] reqbufs;
  if (mr_) {
    char *buf = (char *)mr_->addr;
    CHECK(!ibv_dereg_mr(mr_));
    delete[] buf;
  }
}

} // namespace buddy::dpu
