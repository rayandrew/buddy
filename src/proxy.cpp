#include <cassert>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_error.h>
#include <doca_log.h>

#include "proxy.h"
#include "util.h"

namespace buddy::dpu {

Proxy::~Proxy()
{}

Proxy::Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients,
    rdma::QP *qps, dma::Engine *dma_engine)
  : config(config)
  , cqs(cqs)
  , num_clients(num_clients)
  , qps(qps)
  , dma_engine(dma_engine)
  , quit(false)
{
  size_t total_size = PROXY_BUF_SIZE * PROXY_RX_DEPTH;
  char *buffer = (char *)malloc(total_size);

  mr = ibv_reg_mr(rdma::Context::get().get_pd(), buffer, total_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!mr) {
    perror("ibv_reg_mr");
    FAIL("failed to reg mr");
  }

  for (unsigned i = 0; i < num_clients; i++)
    qp_num_to_idx[qps[i].get_qp()->qp_num] = i;

  for (uint64_t wr_id = 0; wr_id < PROXY_RX_DEPTH; wr_id++) {
    post_recv(wr_id);
  }
}

// As this is not latency-critical, we could use completion events to save cpu
void Proxy::harvest_wcs()
{
  while (!quit) {
    ibv_wc wc[PROXY_RX_DEPTH];
    int n;

    do {
      n = ibv_poll_cq(cqs.send, PROXY_RX_DEPTH, wc);
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

  uint64_t *hist_reqs = (uint64_t*)calloc(PROXY_RX_DEPTH, sizeof(uint64_t));

  if (pthread_create(&harvest_thread, NULL, run_harvest_thread, this) < 0)
    FAIL("failed to create thread");

  while (!quit) {
    ibv_wc wc[PROXY_RX_DEPTH];

    std::cout << "poll..." << std::endl;

    int n;
    do {
      n = ibv_poll_cq(cqs.recv, PROXY_RX_DEPTH, wc);
    } while (n == 0 && !quit);

    CHECK(n >= 0);

    if (n > 0)
      hist_reqs[n-1]++;

    for (int i = 0; i < n; i++) {
      CHECK(wc[i].status == IBV_WC_SUCCESS);
      CHECK(wc[i].opcode == IBV_WC_RECV);
      CHECK(wc[i].wc_flags & IBV_WC_WITH_IMM);

      unsigned client_idx = qp_num_to_idx[wc[i].qp_num];
      uint32_t imm_tag = wc[i].imm_data;

      uint64_t wr_id = wc[i].wr_id;
      char *recv_buf = (char *)mr->addr + wr_id * PROXY_BUF_SIZE;
      size_t msglen = wc[i].byte_len;

      switch (imm_tag) {
        case 0:
          {
            CHECK(msglen == sizeof(uint64_t));
            uint64_t *dmalen = (uint64_t *)recv_buf;
            std::cout << "Got msg from client " << client_idx << 
              "dmalen = " << *dmalen << std::endl;

            dma_engine->transfer(client_idx, *dmalen, dma::H2D);
            std::cout << "initiated dma" << std::endl;

            unsigned cl;
            dma::direction dir;
            while (!dma_engine->poll(&cl, &dir));

            CHECK(cl == client_idx);
            CHECK(dir == dma::H2D);

            char *buf = dma_engine->client_buf(client_idx);
            buf[*dmalen] = 0;
            std::cout << "dma message: " << buf << std::endl;

            break;
          }

        default:
          FAIL("unknown imm_tag for recv ");
          break;
      }

      post_recv(wc[i].wr_id);
    }
  }

  printf("hist_reqs = [");
  for (uint64_t i = 0; i < PROXY_RX_DEPTH; i++) {
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
    assert(wr_id < PROXY_RX_DEPTH);

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

} // namespace buddy::dpu
