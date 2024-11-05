#include <cassert>
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

  std::cout << "--- network bytes ---" << std::endl;
  std::cout << "in_local\t" << count_in_local << std::endl;
  std::cout << "in_remote\t" << count_in_remote << std::endl;
  std::cout << "out_local\t" << count_out_local << std::endl;
  std::cout << "out_remote\t" << count_out_remote << std::endl;
  std::cout << "---------------------" << std::endl;
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
  , quit(false)
  , rx_depth(4*num_clients)
  , local_idx_to_rank(ranks)
  , d2h_send(num_clients, config.d2h_size)
  // TODO: allow different d2d buffer size
  //, d2d_send(num_remotes, D2D_SIZE)
  , d2d_send(num_remotes, config.h2d_size)
  , routing_table(routing_table)
{
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

void Proxy::harvest_wcs()
{
  ibv_wc wc[rx_depth];

  int n = ibv_poll_cq(cqs.send, rx_depth, wc);

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
    route rt = route::from_int(wr_id);

    auto send_bufs = &d2h_send;
    if (rt.remote)
      send_bufs = &d2d_send;

    assert(send_bufs->is_flushing(rt.idx));
    send_bufs->reqs(rt.idx).reset_pos();
    send_bufs->set_flushing(rt.idx, false);
  }
}

void *run_harvest_thread(void *arg)
{
  pthread_setname_np(pthread_self(), "buddy-harvest");

  Proxy *proxy = (Proxy *)arg;
  while (!proxy->quit)
    proxy->harvest_wcs();

  return NULL;
}

void Proxy::rdma_loop()
{
  uint64_t *hist_reqs = (uint64_t*)calloc(rx_depth, sizeof(uint64_t));

  /*
  pthread_t harvest_thread;
  if (pthread_create(&harvest_thread, NULL, run_harvest_thread, this) < 0)
    FAIL("failed to create thread");
  */

  unsigned quit_counter = 0;
  bool pending_flush = false;

  tic(TT_RDMALOOP);

  // Tiny opt: only check atomic quit flag every X iterations
  while (!quit) {
    ibv_wc wc[rx_depth];

    tic(TT_POLL);

    int n = ibv_poll_cq(cqs.recv, rx_depth, wc);
    CHECK(n >= 0);

    if (!n && pending_flush) {
      flush_all();
      pending_flush = false;
    }

    while (n == 0 && !quit) {
      n = ibv_poll_cq(cqs.recv, rx_depth, wc);
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
            if (quit_counter == num_clients)
              quit = true;
            break;
          }

        case IMM_H2D_RDMA:
          {
            TRACE("recv H2D_RDMA size " << msglen);
            count_in_local += msglen;
            route_reqs(recv_buf, msglen);
            pending_flush = true;
            break;
          }

        case IMM_D2D_RDMA:
          {
            TRACE("recv D2D_RDMA size " << msglen);
            count_in_remote += msglen;
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

    harvest_wcs();
  }

  toc(TT_RDMALOOP);

  printf("hist_reqs = [");
  for (uint64_t i = 0; i < rx_depth; i++) {
    printf("%lu,", hist_reqs[i]);
  }
  printf("]\n");

  /*
  if (pthread_join(harvest_thread, NULL))
    FAIL("join harvest thread failed");
  */

  free(hist_reqs);
  
  std::cout << "--- proxy breakdown" << " ---" << std::endl;
  tt_print();
}

void Proxy::post_recv(uint64_t wr_id)
{
  assert(wr_id >= 0);
  assert(wr_id < rx_depth);

  uint64_t offset = wr_id * config.h2d_size;
  uint64_t h2d_buf = (uint64_t) h2d_mr->addr;

  struct ibv_sge list = {
    .addr = h2d_buf + offset,
    .length = config.h2d_size,
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

  ReqBufRead reader(buf, len);

  request_head *head;
  char *data;
  while (reader.next(&head, &data, -1)) {
    CHECK(head->dst >= 0 && head->dst < world_size);
    route r = routing_table[head->dst];

    auto clock = TT_D2HBLOCK;
    auto send_bufs = &d2h_send;
    size_t bytes = sizeof(request_head) + head->size;
    if (r.remote) {
      count_out_remote += bytes;
      clock = TT_D2DBLOCK;
      send_bufs = &d2d_send;
      TRACE("route to " << head->dst << " (remote " << r.idx << ")");
    } else {
      count_out_local += bytes;
      TRACE("route to " << head->dst << " (local " << r.idx << ")");
    }

    if (send_bufs->is_flushing(r.idx)) {
      tic(clock);
      do
        harvest_wcs();
      while (send_bufs->is_flushing(r.idx));
      toc(clock);
    }

    ReqBufWrite& req = send_bufs->reqs(r.idx);
    if (!req.append(*head, data)) {
      tic(TT_APPBLOCK);
      do {
        if (send_bufs->is_flushing(r.idx))
          harvest_wcs();
        else
          flush(r);
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
  assert(!d2d_send.is_flushing(idx));

  if (d2d_send.reqs(idx).empty())
    return;

  tic(TT_REMFLUSH);

  d2d_send.set_flushing(idx, true);

  size_t size = d2d_send.reqs(idx).get_pos();
  TRACE("send to remote " << idx << " size " << size);
  remote_qps[idx].send_imm(IMM_D2D_RDMA, d2d_send.mr(),
      size, d2d_send.offset(idx),
      route::make_remote(idx).as_int());

  toc(TT_REMFLUSH);
}

void Proxy::flush_local(unsigned idx)
{
  assert(!d2h_send.is_flushing(idx));

  if (d2h_send.reqs(idx).empty())
    return;

  tic(TT_LOCFLUSH);

  d2h_send.set_flushing(idx, true);

  size_t size = d2h_send.reqs(idx).get_pos();
  TRACE("send to local rank " << local_idx_to_rank[idx] << " size " << size);
  local_qps[idx].send_imm(IMM_D2H_RDMA, d2h_send.mr(), size,
      d2h_send.offset(idx), route::make_local(idx).as_int());

  toc(TT_LOCFLUSH);
}

void Proxy::flush_all()
{
  for (unsigned i = 0; i < num_clients; i++)
    if (!d2h_send.is_flushing(i))
      flush_local(i);

  for (unsigned i = 0; i < num_remotes; i++)
    if (!d2d_send.is_flushing(i))
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

  //flushing = new std::atomic_bool[n];
  flushing = new bool[n];
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
