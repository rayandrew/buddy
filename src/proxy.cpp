#include <cassert>
#include <cmath>
#include <omp.h>
#include "proxy.h"
#include "util.h"
#include "util_mpi.h"
#include "local_proto.h"
#include "valgrind/memcheck.h"

#ifdef NDEBUG
#define TRACE(l, x) do{}while(0)
#else
#define TRACE(l, x) do{if (config.trace >= (l)){std::clog << x << std::endl;}}while(0)
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

// send-CQ wr_id tags. Flush wr_ids are send_buf_ids (top byte 0, repid is small), so these
// reserved top bytes let poll_send_queue tell reads/control sends apart from flushes.
static const uint64_t WRID_READ = 0xFFull << 56;   // low 32 bits = recv slot idx
static const uint64_t WRID_CTRL = 0xFEull << 56;   // READY/ACK send; nothing to free

Proxy::~Proxy()
{
  char *h2d_buf = (char *)h2d_mr->addr;
  CHECK(!ibv_dereg_mr(h2d_mr));
  delete[] h2d_buf;

  if (d2d_mr) {
    char *d2d_buf = (char *)d2d_mr->addr;
    CHECK(!ibv_dereg_mr(d2d_mr));
    delete[] d2d_buf;
  }
}

int get_num_threads()
{
  int num_threads = omp_get_num_threads();

#pragma omp parallel
#pragma omp master
  {
    num_threads = omp_get_num_threads();
  }

  return num_threads;
}

Proxy::Proxy(ProxyConfig config, proxy_cqs cqs, unsigned num_clients,
    unsigned num_remotes, int world_size,
    rdma::QP *local_qps, rdma::QP *remote_qps,
    int *ranks, route *routing_table)
  : config(config)
  , num_clients(num_clients)
  , num_remotes(num_remotes)
  , world_size(world_size)
  , cqs(cqs)
  , local_qps(local_qps)
  , remote_qps(remote_qps)
  , num_threads(get_num_threads())
  , h2d_depth(config.bufcount_local*num_clients*num_threads)
  , d2d_depth(config.bufcount_remote*num_remotes*num_threads)
  , rx_depth(h2d_depth + d2d_depth)
  , local_idx_to_rank(ranks)
  , routing_table(routing_table)
{
  in_counters = new rdma_counters[num_threads];

  d2h_send = new SendBufs[num_threads];
  d2d_send = new SendBufs[num_threads];
  last_thread_progress = new double[num_threads];
  pending_reads = new pending_read[d2d_depth + 1];

  for (int tid = 0; tid < num_threads; tid++) {
    new (&d2h_send[tid]) SendBufs(num_clients, config.bufcount_local, config.d2h_size);
    new (&d2d_send[tid]) SendBufs(num_remotes, config.bufcount_remote, config.d2d_size);
  }

  size_t total_size_h2d = config.h2d_size * h2d_depth;
  CHECK(total_size_h2d);
  char *h2d_buf = new char[total_size_h2d];
  h2d_mr = ibv_reg_mr(rdma::Context::get().get_pd(), h2d_buf, total_size_h2d, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  CHECK(h2d_mr);

  size_t total_size_d2d = config.d2d_size * d2d_depth;
  CHECK(!num_remotes || total_size_d2d);
  if (total_size_d2d) {
    char *d2d_buf = new char[total_size_d2d];
    d2d_mr = ibv_reg_mr(rdma::Context::get().get_pd(), d2d_buf, total_size_d2d, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    CHECK(d2d_mr);
  } else {
    d2d_mr = nullptr;
  }

  // write mode: a separate landing region (peers RDMA_WRITE here); one disjoint sub-region of
  // slots_per_peer slots per peer, mirroring each sender's per-thread bufcount buffers.
  if (config.d2d_mode == D2D_WRITE && num_remotes) {
    slots_per_peer = config.bufcount_remote * num_threads;
    size_t total_landing = (size_t)config.d2d_size * slots_per_peer * num_remotes;
    char *landing_buf = new char[total_landing];
    landing_mr = ibv_reg_mr(rdma::Context::get().get_pd(), landing_buf, total_landing,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    CHECK(landing_mr);
    peer_landing = new peer_land[num_remotes]();
  }

  std::cout << "ranks:";
  for (unsigned i = 0; i < num_clients; i++)
    std::cout << " " << ranks[i];
  std::cout << std::endl;

  for (uint32_t i = 0; i < h2d_depth; i++)
    post_recv(route::make_local(i));

  for (uint32_t i = 0; i < d2d_depth; i++)
    post_recv(route::make_remote(i));

  TRACE(1, "trace on");
}

// write mode startup: advertise our landing sub-region to each peer and collect theirs.
void Proxy::d2d_write_exchange()
{
  for (unsigned i = 0; i < num_remotes; i++) {
    d2d_desc adv = {
      .addr = (uint64_t)((char *)landing_mr->addr + (uint64_t)i * slots_per_peer * config.d2d_size),
      .id   = (uint64_t)(i * slots_per_peer),   // base slot for peer i
      .rkey = landing_mr->rkey,
      .len  = slots_per_peer,
    };
    remote_qps[i].send_imm_inline(IMM_D2D_MRINFO, &adv, sizeof(adv), 0, WRID_CTRL);
  }

  for (unsigned got = 0; got < num_remotes; ) {
    ibv_wc wc;
    int n = ibv_poll_cq(cqs.remote_recv, 1, &wc);
    CHECK(n >= 0);
    if (!n)
      continue;
    CHECK(wc.status == IBV_WC_SUCCESS);
    CHECK((wc.imm_data & IMM_TAG_MASK) == IMM_D2D_MRINFO);
    unsigned peer = remote_idx_of(wc.qp_num);
    route rt = route::from_int(wc.wr_id);
    d2d_desc adv;
    memcpy(&adv, get_recv_buf(rt), sizeof(adv));
    peer_landing[peer] = { adv.addr, adv.rkey, (uint32_t)adv.id, adv.len };
    post_recv(rt);
    got++;
  }
}

unsigned Proxy::remote_idx_of(uint32_t qp_num)
{
  for (unsigned i = 0; i < num_remotes; i++)
    if (remote_qps[i].get_qp()->qp_num == qp_num)
      return i;
  FAIL("unknown remote qp_num " << qp_num);
}

void Proxy::handle_read_complete(uint32_t slot, std::list<blocked_req>& blocklist)
{
  int tid = omp_get_thread_num();
  auto &pr = pending_reads[slot];
  route recv_rt = route::make_remote(slot);

  in_counters[tid].count_remote++;
  in_counters[tid].bytes_remote += pr.len;
  TRACE(1, "read done slot " << slot << " size " << pr.len << " peer " << pr.peer);

  // Route via the blocklist drain (which acks + reposts) to avoid recursing through route_reqs.
  blocked_req br = {recv_rt, ReqBufRead(get_recv_buf(recv_rt), pr.len),
      omp_get_wtime() + config.timeout, true, pr.peer, pr.desc_id};
  blocklist.push_back(br);
}

void Proxy::poll_send_queue(std::list<blocked_req>& blocklist)
{
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
    uint8_t tag = wr_id >> 56;
    if (tag == (WRID_CTRL >> 56))            // READY/ACK send done; nothing to free
      continue;
    if (tag == (WRID_READ >> 56)) {          // pull read completed
      handle_read_complete((uint32_t)(wr_id & 0xFFFFFFFF), blocklist);
      continue;
    }

    auto id = send_buf_id::from_int(wr_id);

    auto send_bufs = &d2h_send[id.tid];
    if (id.rt.remote)
      send_bufs = &d2d_send[id.tid];

    send_bufs->mark_complete(id.rt.idx, id.repid);
  }
}

bool Proxy::poll_recv_queue(std::list<blocked_req>& blocklist)
{
  // To avoid deadlock situation where a d2d recv is blocked by earlier h2d
  // recvs we should only take 1 wc from the queue at once.
  // TODO: not relevant anymore.
  ibv_wc wc;

  tic(TT_POLL);

  // Prioritize remote recvs first. To rate limit fast hosts.
  int n = ibv_poll_cq(cqs.remote_recv, 1, &wc);
  CHECK(n >= 0);

  if (!n) {
    n = ibv_poll_cq(cqs.local_recv, 1, &wc);
    CHECK(n >= 0);
  }

  if (!n)
    return false;
  else
    assert(n == 1);

  toc(TT_POLL);

  CHECK(wc.status == IBV_WC_SUCCESS);
  CHECK(wc.opcode == IBV_WC_RECV || wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM);
  CHECK(wc.wc_flags & IBV_WC_WITH_IMM);

  uint32_t imm_tag = wc.imm_data & IMM_TAG_MASK;   // IMM_D2D_WRITE packs a slot in the high bits
  switch (imm_tag) {
    case IMM_QUIT:
      {
        quit_counter++;
        break;
      }

    case IMM_H2D_RDMA:
    case IMM_D2D_RDMA:
      {
        uint64_t wr_id = wc.wr_id;
        route recv_rt = route::from_int(wr_id);

        char *recv_buf = get_recv_buf(recv_rt);
        size_t msglen = wc.byte_len;
        VALGRIND_MAKE_MEM_DEFINED(recv_buf, msglen);

        int tid = omp_get_thread_num();

        if (imm_tag == IMM_H2D_RDMA) {
          TRACE(1, "recv H2D_RDMA size " << msglen);
          assert(!recv_rt.remote);

          in_counters[tid].count_local++;
          in_counters[tid].bytes_local += msglen;
        } else {
          TRACE(1, "recv D2D_RDMA size " << msglen);
          assert(recv_rt.remote);

          in_counters[tid].count_remote++;
          in_counters[tid].bytes_remote += msglen;
        }

        ReqBufRead reader(recv_buf, msglen);
        if (route_reqs(reader, blocklist))
          post_recv(recv_rt);
        else {
          blocked_req br = {recv_rt, reader, omp_get_wtime() + config.timeout};
          blocklist.push_back(br);
        }

        break;
      }

    case IMM_D2D_READY:
      {
        // pull: descriptor landed in this recv slot; read the bulk into the same slot.
        route recv_rt = route::from_int(wc.wr_id);
        char *slotbuf = get_recv_buf(recv_rt);
        d2d_desc desc;
        memcpy(&desc, slotbuf, sizeof(desc));
        unsigned peer = remote_idx_of(wc.qp_num);
        pending_reads[recv_rt.idx] = { peer, desc.id, desc.len };
        TRACE(1, "recv D2D_READY size " << desc.len << " peer " << peer);
        remote_qps[peer].read(slotbuf, d2d_mr, desc.len, desc.addr, desc.rkey,
            WRID_READ | recv_rt.idx);
        break;
      }

    case IMM_D2D_ACK:
      {
        // pull: peer finished reading our buffer; free it and re-arm this slot.
        route recv_rt = route::from_int(wc.wr_id);
        uint64_t freed;
        memcpy(&freed, get_recv_buf(recv_rt), sizeof(freed));
        auto id = send_buf_id::from_int(freed);
        d2d_send[id.tid].mark_complete(id.rt.idx, id.repid);
        post_recv(recv_rt);
        TRACE(1, "recv D2D_ACK free tid " << id.tid << " idx " << id.rt.idx);
        break;
      }

    case IMM_D2D_WRITE:
      {
        // write: data already landed in landing_mr[abs_slot]; the consumed recv slot is a dummy.
        uint32_t abs_slot = wc.imm_data >> IMM_SLOT_SHIFT;
        unsigned peer = remote_idx_of(wc.qp_num);
        route recv_rt = route::from_int(wc.wr_id);
        char *buf = (char *)landing_mr->addr + (uint64_t)abs_slot * config.d2d_size;
        size_t msglen = wc.byte_len;
        int tid = omp_get_thread_num();
        in_counters[tid].count_remote++;
        in_counters[tid].bytes_remote += msglen;
        TRACE(1, "recv D2D_WRITE slot " << abs_slot << " size " << msglen << " peer " << peer);

        ReqBufRead reader(buf, msglen);
        if (route_reqs(reader, blocklist)) {
          remote_qps[peer].send_imm_inline(IMM_D2D_CREDIT, &abs_slot, sizeof(abs_slot), 0, WRID_CTRL);
          post_recv(recv_rt);
        } else {
          blocked_req br = {recv_rt, reader, omp_get_wtime() + config.timeout, true, peer, abs_slot};
          blocklist.push_back(br);
        }
        break;
      }

    case IMM_D2D_CREDIT:
      {
        // write: peer freed a landing slot; free the matching send buffer.
        route recv_rt = route::from_int(wc.wr_id);
        uint32_t abs_slot;
        memcpy(&abs_slot, get_recv_buf(recv_rt), sizeof(abs_slot));
        unsigned peer = remote_idx_of(wc.qp_num);
        unsigned s = abs_slot - peer_landing[peer].base_slot;
        d2d_send[s / config.bufcount_remote].mark_complete(peer, s % config.bufcount_remote);
        post_recv(recv_rt);
        TRACE(1, "recv D2D_CREDIT slot " << abs_slot);
        break;
      }

    default:
      FAIL("unknown imm_tag for recv " << imm_tag);
      break;
  }

  return true;
}

void Proxy::rdma_loop()
{
  quit_counter = 0;

  if (config.d2d_mode == D2D_WRITE && num_remotes)
    d2d_write_exchange();

  uint64_t max_blocked = 0;
  uint64_t quiet_flush_events = 0;
  uint64_t quiet_flush_bufs = 0;

#pragma omp parallel reduction(max:max_blocked) reduction(+:quiet_flush_events,quiet_flush_bufs)
  {
    int tid = omp_get_thread_num();
    last_thread_progress[tid] = omp_get_wtime();

    std::list<blocked_req> blocklist;

#pragma omp barrier
#pragma omp single
    {
      std::cout << num_threads << " rdma threads ready" << std::endl;
    }

    double last_recv = omp_get_wtime();

    tic(TT_RDMALOOP);

    // Logic to drain cq after quit should not be necessary if quit is
    // preceeded by an application barrier.
    while (quit_counter != num_clients) {
      bool recv = poll_recv_queue(blocklist);

      if (recv) {
        last_recv = omp_get_wtime();
      } else if (config.quiet_time >= 0) {
        double now = omp_get_wtime();
        if (now > last_recv + config.quiet_time) {
          last_recv = now;
          size_t num_flushed = flush_all();
          if (num_flushed) {
            TRACE(1, "quiet time elapsed, all flushed!");
            quiet_flush_events++;
            quiet_flush_bufs += num_flushed;
          }
        }

        // Thread is idle, no work to do
        if (blocklist.empty())
          last_thread_progress[tid] = now;
      }

      // std::list::size is constant since C++11
      max_blocked = std::max(blocklist.size(), max_blocked);

      auto req = blocklist.begin();
      while (req != blocklist.end()) {
        if (route_reqs(req->reqbuf, blocklist)) {
          if (req->needs_ack) {
            if (config.d2d_mode == D2D_WRITE) {
              uint32_t fs = (uint32_t)req->ack_id;
              remote_qps[req->ack_peer].send_imm_inline(IMM_D2D_CREDIT, &fs, sizeof(fs), 0, WRID_CTRL);
            } else {
              remote_qps[req->ack_peer].send_imm_inline(IMM_D2D_ACK, &req->ack_id,
                  sizeof(req->ack_id), 0, WRID_CTRL);
            }
          }
          post_recv(req->recv_rt);
          req = blocklist.erase(req);
        } else
          ++req;
      }

      double now = omp_get_wtime();
      if (now > last_thread_progress[tid] + config.timeout) {
        FAIL("Thread " << tid << " timed out, no progress for " << config.timeout << " s."
            << " Length of blocklist is " << blocklist.size() << ".");
      }

      poll_send_queue(blocklist);
    }

    toc(TT_RDMALOOP);
  }

  int rank;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));

  uint64_t max_blocked_global;
  CHECK_MPI(MPI_Reduce(&max_blocked, &max_blocked_global, 1, MPI_UINT64_T, MPI_MAX, 0, MPI_COMM_WORLD));
  if (!rank)
    std::cout << "max_blocked_thread\t" << max_blocked_global << std::endl;

  uint64_t flush_events_global;
  uint64_t flush_bufs_global;
  CHECK_MPI(MPI_Reduce(&quiet_flush_events, &flush_events_global, 1, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD));
  CHECK_MPI(MPI_Reduce(&quiet_flush_bufs, &flush_bufs_global, 1, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD));
  if (!rank) {
    std::cout << "quiet_flush_events\t" << flush_events_global << std::endl;
    std::cout << "quiet_flush_bufs\t" << flush_bufs_global << std::endl;
  }

  print_counters();
}

void SizeHistogram::record(size_t size)
{
  size_t level = 0;

  if (size > 0) {
    double dlevel = log10(size);
    if (dlevel <= 0.0) {
      level = 0;
    } else {
      level = (size_t)dlevel;
      if (level >= levels)
        level = levels-1;
    }
  }

  counts[level] += 1;
}

void SizeHistogram::add(const SizeHistogram& other)
{
  for (unsigned level = 0; level < levels; level++)
    counts[level] += other.counts[level];
}

void SizeHistogram::reduce()
{
  uint64_t send[levels];
  for (unsigned level = 0; level < levels; level++)
    send[level] = counts[level];

  CHECK_MPI(MPI_Reduce(send, counts, levels, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD));
}

void SizeHistogram::print()
{
  for (unsigned level = 0; level < levels; level++)
    std::cout << counts[level] << '\t';
  std::cout << std::endl;
}

uint64_t avg(uint64_t bytes, uint64_t count)
{
  if (count)
    return bytes/count;
  else
    return 0;
}

void Proxy::print_counters()
{
  rdma_counters in_total, out_total;
  for (int tid = 0; tid < num_threads; tid++) {
    rdma_counters thread_out;
    d2h_send[tid].get_counts(&thread_out.count_local, &thread_out.bytes_local);
    d2d_send[tid].get_counts(&thread_out.count_remote, &thread_out.bytes_remote);

    out_total.count_local += thread_out.count_local;
    out_total.count_remote += thread_out.count_remote;
    out_total.bytes_local += thread_out.bytes_local;
    out_total.bytes_remote += thread_out.bytes_remote;

    in_total.count_local += in_counters[tid].count_local;
    in_total.count_remote += in_counters[tid].count_remote;
    in_total.bytes_local += in_counters[tid].bytes_local;
    in_total.bytes_remote += in_counters[tid].bytes_remote;
  }

  int rank;
  CHECK_MPI(MPI_Comm_rank(MPI_COMM_WORLD, &rank));

  rdma_counters in_send = in_total, out_send = out_total;
  CHECK_MPI(MPI_Reduce(&in_send, &in_total, sizeof(in_total) / sizeof(uint64_t), MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD));
  CHECK_MPI(MPI_Reduce(&out_send, &out_total, sizeof(out_total) / sizeof(uint64_t), MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD));

  if (rank == 0) {
    std::cout << "--- msg counts ---" << std::endl;
    std::cout << "count_in_local\t" << in_total.count_local << std::endl;
    std::cout << "count_in_remote\t" << in_total.count_remote << std::endl;
    std::cout << "count_out_local\t" << out_total.count_local << std::endl;
    std::cout << "count_out_remote\t" << out_total.count_remote << std::endl;
    std::cout << "------------------" << std::endl;

    std::cout << "--- network bytes ---" << std::endl;
    std::cout << "bytes_in_local\t" << in_total.bytes_local << std::endl;
    std::cout << "bytes_in_remote\t" << in_total.bytes_remote << std::endl;
    std::cout << "bytes_out_local\t" << out_total.bytes_local << std::endl;
    std::cout << "bytes_out_remote\t" << out_total.bytes_remote << std::endl;
    std::cout << "---------------------" << std::endl;

    std::cout << "--- avg msg size ---" << std::endl;
    std::cout << "avg_in_local\t" << avg(in_total.bytes_local, in_total.count_local) << std::endl;
    std::cout << "avg_in_remote\t" << avg(in_total.bytes_remote, in_total.count_remote) << std::endl;
    std::cout << "avg_out_local\t" << avg(out_total.bytes_local, out_total.count_local) << std::endl;
    std::cout << "avg_out_remote\t" << avg(out_total.bytes_remote, out_total.count_remote) << std::endl;
    std::cout << "--------------------" << std::endl;
  }

  SizeHistogram d2h_hist, d2d_hist;
  for (int tid = 0; tid < num_threads; tid++) {
    d2h_hist.add(d2h_send[tid].get_hist());
    d2d_hist.add(d2d_send[tid].get_hist());
  }

  d2h_hist.reduce();
  d2d_hist.reduce();

  if (rank == 0) {
    std::cout << "--- local size histogram ---" << std::endl;
    d2h_hist.print();

    std::cout << "--- remote size histogram ---" << std::endl;
    d2d_hist.print();
  }

  tt_print_mpi("proxy breakdown");
}

char *Proxy::get_recv_buf(route rt)
{
  uint32_t len;
  ibv_mr *mr;

  if (rt.remote) {
    mr = d2d_mr;
    len = config.d2d_size;
  } else {
    mr = h2d_mr;
    len = config.h2d_size;
  }

  char *buf = (char *)mr->addr + rt.idx * len;
  return buf;
}

void Proxy::post_recv(route rt)
{
  assert(rt.idx < rx_depth);

  uint32_t len;
  uint32_t lkey;
  ibv_srq *srq;

  if (rt.remote) {
    assert(rt.idx < d2d_depth);

    lkey = d2d_mr->lkey;
    len = config.d2d_size;
    srq = cqs.remote_srq;
  } else {
    assert(rt.idx < h2d_depth);

    lkey = h2d_mr->lkey;
    len = config.h2d_size;
    srq = cqs.local_srq;
  }

  struct ibv_sge list = {
    .addr   = (uint64_t)get_recv_buf(rt),
    .length = (uint32_t)len,
    .lkey   = lkey
  };
  VALGRIND_MAKE_MEM_UNDEFINED(list.addr, list.length);

  struct ibv_recv_wr *bad_wr;
  struct ibv_recv_wr wr = {
    .wr_id = rt.as_int(),
    .next       = NULL,
    .sg_list    = &list,
    .num_sge    = 1,
  };

  CHECK_ERR(ibv_post_srq_recv(srq, &wr, &bad_wr));
}

bool Proxy::route_reqs(ReqBufRead &reader, std::list<blocked_req>& blocklist)
{
  tic(TT_ROUTE);

  int tid = omp_get_thread_num();

  bool complete = true;
  bool progress = false;

  request_head *head;
  char *data;
  while (reader.peek(&head, &data)) {
    CHECK(head->dst >= 0 && head->dst < world_size);
    route r = routing_table[head->dst];

    auto send_bufs = &d2h_send[tid];
    if (r.remote)
      send_bufs = &d2d_send[tid];

    int repid = send_bufs->get_ready_repid(r.idx);
    while (repid >= 0) {
      ReqBufWrite& req = send_bufs->reqs(r.idx, repid);
      if (!req.append(*head, data)) {
        flush(r, repid);
        repid = send_bufs->get_ready_repid(r.idx);
      } else {
        if (r.remote)
          TRACE(2, "route to " << head->dst << " (remote " << r.idx << ")");
        else
          TRACE(2, "route to " << head->dst << " (local " << r.idx << ")");

        reader.advance(head);
        progress = true;
        break;
      }
    }

    if (repid < 0) {
      // No free send buffers
        if (r.remote)
          TRACE(2, "failed route to " << head->dst << " (remote " << r.idx << ")");
        else
          TRACE(2, "failed route to " << head->dst << " (local " << r.idx << ")");

      poll_send_queue(blocklist);
      complete = false;
      break;
    }
  }

  if (progress)
    last_thread_progress[tid] = omp_get_wtime();

  toc(TT_ROUTE);

  return complete;
}

bool Proxy::flush(route r, unsigned repid)
{
  if (r.remote)
    return flush_remote(r.idx, repid);
  else
    return flush_local(r.idx, repid);
}

bool Proxy::flush_remote(unsigned idx, unsigned repid)
{
  int tid = omp_get_thread_num();

  if (d2d_send[tid].reqs(idx, repid).empty())
    return false;

  tic(TT_REMFLUSH);

  d2d_send[tid].mark_flushing(idx, repid);

  size_t size = d2d_send[tid].reqs(idx, repid).get_pos();
  route rt = route::make_remote(idx);
  send_buf_id id = {rt, (uint16_t)tid, (uint16_t)repid};

  TRACE(1, "send to remote " << idx << " size " << size << " thread " << tid);
  char *buf = (char *)d2d_send[tid].mr()->addr + d2d_send[tid].offset(idx, repid);
  if (config.d2d_mode == D2D_READ) {
    // pull: advertise the buffer; it stays FLUSHING until the peer reads it and acks.
    d2d_desc desc = { .addr = (uint64_t)buf, .id = id.as_int(),
        .rkey = d2d_send[tid].mr()->rkey, .len = (uint32_t)size };
    remote_qps[idx].send_imm_inline(IMM_D2D_READY, &desc, sizeof(desc), 0, WRID_CTRL);
  } else if (config.d2d_mode == D2D_WRITE) {
    // push one-sided into our dedicated landing slot; stays FLUSHING until CREDIT frees it.
    unsigned s = tid * config.bufcount_remote + repid;
    CHECK(s < peer_landing[idx].num_slots);
    uint32_t abs_slot = peer_landing[idx].base_slot + s;
    uint64_t raddr = peer_landing[idx].addr + (uint64_t)s * config.d2d_size;
    remote_qps[idx].write_imm((abs_slot << IMM_SLOT_SHIFT) | IMM_D2D_WRITE,
        buf, d2d_send[tid].mr(), size, raddr, peer_landing[idx].rkey, WRID_CTRL);
  } else {
    remote_qps[idx].send_imm(IMM_D2D_RDMA, d2d_send[tid].mr(),
        size, d2d_send[tid].offset(idx, repid), id.as_int());
  }

  toc(TT_REMFLUSH);

  return true;
}

bool Proxy::flush_local(unsigned idx, unsigned repid)
{
  int tid = omp_get_thread_num();

  if (d2h_send[tid].reqs(idx, repid).empty())
    return false;

  tic(TT_LOCFLUSH);

  d2h_send[tid].mark_flushing(idx, repid);

  size_t size = d2h_send[tid].reqs(idx, repid).get_pos();
  route rt = route::make_local(idx);
  send_buf_id id = {rt, (uint16_t)tid, (uint16_t)repid};

  TRACE(1, "send to local rank " << local_idx_to_rank[idx] << " (idx " << idx << ")" << " size " << size << " thread " << tid);
  local_qps[idx].send_imm(IMM_D2H_RDMA, d2h_send[tid].mr(), size,
      d2h_send[tid].offset(idx, repid), id.as_int());

  toc(TT_LOCFLUSH);

  return true;
}

size_t Proxy::flush_all()
{
  int tid = omp_get_thread_num();
  size_t num_flushed = 0;

  for (unsigned i = 0; i < num_clients; i++)
    for (unsigned j = 0; j < config.bufcount_local; j++)
      if (d2h_send[tid].ready_for_send(i, j))
        if (flush_local(i, j))
          num_flushed++;

  for (unsigned i = 0; i < num_remotes; i++)
    for (unsigned j = 0; j < config.bufcount_remote; j++)
      if (d2d_send[tid].ready_for_send(i, j))
        if (flush_remote(i, j))
          num_flushed++;

  return num_flushed;
}

SendBufs::SendBufs(unsigned n, unsigned m, unsigned size)
  : n(n)
  , m(m)
  , size(size)
{
  CHECK(m);

  if (!n || !m)
    return;

  reqbufs = new ReqBufWrite[n*m];
  char *buf = new char[n*m*size];
  // REMOTE_READ so the peer DPU can pull from d2d_send buffers in pull mode.
  mr_ = ibv_reg_mr(rdma::Context::get().get_pd(), buf, n*m*size,
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
  CHECK_ERRNO(mr_);

  for (unsigned i = 0; i < n; i++)
    for (unsigned j = 0; j < m; j++)
      new (&reqbufs[idx(i, j)]) ReqBufWrite(buf + offset(i, j), size);

  is_ready = new bool[n*m];
  for (unsigned i = 0; i < n*m; i++)
    is_ready[i] = true;

  is_complete = new std::atomic_bool[n*m]{};

  repid_ptrs = new unsigned[n]{};
}

SendBufs::~SendBufs()
{
  if (is_ready)
    delete[] is_ready;
  if (is_complete)
    delete[] is_complete;
  if (reqbufs)
    delete[] reqbufs;
  if (mr_) {
    char *buf = (char *)mr_->addr;
    CHECK(!ibv_dereg_mr(mr_));
    delete[] buf;
  }
  if (repid_ptrs)
    delete[] repid_ptrs;
}

} // namespace buddy::dpu
