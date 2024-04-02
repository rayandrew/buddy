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

Proxy::Proxy(ProxyConfig config, rdma::server_cqs cqs, unsigned num_clients, rdma::QP *qps)
  : config(config)
  , cqs(cqs)
  , num_clients(num_clients)
  , qps(qps)
  , quit(false)
{
  size_t total_size = PROXY_BUF_SIZE * PROXY_RX_DEPTH;
  char *buffer = (char *)malloc(total_size);

  mr = ibv_reg_mr(rdma::Context::get().get_pd(), buffer, total_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!mr) {
    perror("ibv_reg_mr");
    FAIL("failed to reg mr");
  }

  //init_doca();

  for (unsigned i = 0; i < num_clients; i++)
    qp_num_to_idx[qps[i].get_qp()->qp_num] = i;

  for (uint64_t wr_id = 0; wr_id < PROXY_RX_DEPTH; wr_id++) {
    post_recv(wr_id);
  }
}

void Proxy::init_doca()
{
  struct doca_devinfo **dev_list;
  uint32_t nb_devs;

  if (doca_devinfo_list_create(&dev_list, &nb_devs) != DOCA_SUCCESS)
    FAIL("doca_devinfo_list_create");

  const char *pci_addr = "03:00.0";

  uint32_t dev_idx;
  for (dev_idx = 0; dev_idx < nb_devs; dev_idx++) {
    /*
        char pci[DOCA_DEVINFO_PCI_ADDR_SIZE+1] = {};
        if (doca_devinfo_get_pci_addr_str(dev_list[dev_idx], pci) != DOCA_SUCCESS)
        FAIL("doca_devinfo_get_pci_addr_str");
        */

    uint8_t is_addr_equal = 0;
    if (doca_devinfo_get_is_pci_addr_equal(dev_list[dev_idx], pci_addr, &is_addr_equal) != DOCA_SUCCESS)
      FAIL("doca_devinfo_get_is_pci_addr_equal");

    // TODO change to dma
    //doca_error_t support = doca_compress_job_get_supported(dev_list[dev_idx], DOCA_DECOMPRESS_DEFLATE_JOB);
    doca_error_t support = DOCA_SUCCESS;

    //printf("%s\t%d\t%d\n", pci, (int)is_addr_equal, support==DOCA_SUCCESS);

    if (is_addr_equal && support == DOCA_SUCCESS)
      break;
  }

  if (dev_idx == nb_devs)
    FAIL("could not find suitable doca device!");

  doca_dev *dev = NULL;
  if (doca_dev_open(dev_list[dev_idx], &dev) != DOCA_SUCCESS)
    FAIL("doca_dev_open");

  doca_devinfo_list_destroy(dev_list);

  if (doca_ctx_dev_add(doca_context, dev) != DOCA_SUCCESS)
    FAIL("doca_ctx_dev_add");

  if (doca_ctx_start(doca_context) != DOCA_SUCCESS)
    FAIL("doca_ctx_start");

  if (doca_workq_create(2*PROXY_RX_DEPTH, &workq) != DOCA_SUCCESS)
    FAIL("doca_workq_create");

  if (doca_ctx_workq_add(doca_context, workq) != DOCA_SUCCESS)
    FAIL("doca_workq_add");

  if (doca_mmap_create(NULL, &buf_mmap) != DOCA_SUCCESS)
    FAIL("doca_mmap_create");

  if (doca_mmap_dev_add(buf_mmap, dev) != DOCA_SUCCESS)
    FAIL("doca_mmap_dev_add");

  if (doca_mmap_set_memrange(buf_mmap, mr->addr, mr->length) != DOCA_SUCCESS)
    FAIL("doca_mmap_set_memrange");

  if (doca_mmap_start(buf_mmap) != DOCA_SUCCESS)
    FAIL("doca_mmap_start");

  if (doca_buf_inventory_create(NULL, 2*PROXY_RX_DEPTH, DOCA_BUF_EXTENSION_NONE, &buf_inv) != DOCA_SUCCESS)
    FAIL("doca_buf_inventory_create");

  if (doca_buf_inventory_start(buf_inv) != DOCA_SUCCESS)
    FAIL("doca_buf_inventory_start");

  recv_buf_doca = (doca_buf **)malloc(sizeof(*recv_buf_doca) * PROXY_RX_DEPTH);
  extra_buf_doca = (doca_buf **)malloc(sizeof(*extra_buf_doca) * PROXY_RX_DEPTH);

  for (uint64_t wr_id = 0; wr_id < PROXY_RX_DEPTH; wr_id++) {
    char *recv_buf = (char *)mr->addr + wr_id * PROXY_BUF_SIZE;
    char *extra_buf = (char *)mr->addr + (PROXY_RX_DEPTH + wr_id) * PROXY_BUF_SIZE;

    if (doca_buf_inventory_buf_by_addr(buf_inv, buf_mmap, recv_buf, PROXY_BUF_SIZE, &recv_buf_doca[wr_id]) != DOCA_SUCCESS)
      FAIL("doca_buf_inventory_buf_by_addr");
    if (doca_buf_inventory_buf_by_addr(buf_inv, buf_mmap, extra_buf, PROXY_BUF_SIZE, &extra_buf_doca[wr_id]) != DOCA_SUCCESS)
      FAIL("doca_buf_inventory_buf_by_addr");
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

      int client_idx = qp_num_to_idx[wc[i].qp_num];
      uint32_t imm_tag = wc[i].imm_data;

      switch (imm_tag) {
        case 0:
          {
            std::cout << "Got msg from client " << client_idx << std::endl;
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
