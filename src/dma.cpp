#include <cassert>
#include <cstdlib>
#include <queue>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_ctx.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_error.h>

#include "dma.h"
#include "util.h"
#include "sockets.h"

// PCI of the DOCA DMA-capable device on each side (override via env).
#define HOST_PCIE_DEFAULT "c3:00.0"
#define DPU_PCIE_DEFAULT  "03:00.0"
#define NUM_DMA_TASKS 4096

namespace buddy::dma {

static const char *host_pci() { const char *e = getenv("BUDDY_HOST_PCI"); return e && *e ? e : HOST_PCIE_DEFAULT; }
static const char *dpu_pci()  { const char *e = getenv("BUDDY_DPU_PCI");  return e && *e ? e : DPU_PCIE_DEFAULT; }

// per-worker DOCA context (one progress engine each -> parallel DMA across proxy threads)
struct dma_worker {
  doca_ctx *ctx = nullptr;
  doca_dma *dma = nullptr;
  doca_pe *pe = nullptr;
  doca_buf_inventory *inv = nullptr;
  std::queue<jobspec> completed;
};

struct buffer_desc {
    void *addr;
    size_t len;
    size_t export_desc_len;
};

static struct doca_dev *open_device(const char *pci_addr)
{
  struct doca_devinfo **dev_list;
  uint32_t nb_devs;
  uint8_t is_addr_equal = 0;

  CHECK_DOCA(doca_devinfo_create_list(&dev_list, &nb_devs));

  for (uint32_t i = 0; i < nb_devs; i++) {
    if (doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr, &is_addr_equal) == DOCA_SUCCESS
        && is_addr_equal) {
      struct doca_dev *dev = NULL;
      if (doca_dev_open(dev_list[i], &dev) == DOCA_SUCCESS) {
        doca_devinfo_destroy_list(dev_list);
        return dev;
      }
    }
  }

  doca_devinfo_destroy_list(dev_list);
  FAIL("no DOCA DMA device at pci " << pci_addr);
}

// ------------------------------------------------------------------- Buffer (host side)
Buffer::Buffer(size_t len)
  : buf(new char[len])
  , len(len)
{
  dev = open_device(host_pci());

  CHECK_DOCA(doca_mmap_create(&dmap));
  CHECK_DOCA(doca_mmap_add_dev(dmap, dev));
  CHECK_DOCA(doca_mmap_set_permissions(dmap, DOCA_ACCESS_FLAG_PCI_READ_WRITE));
  CHECK_DOCA(doca_mmap_set_memrange(dmap, buf, len));
  CHECK_DOCA(doca_mmap_start(dmap));
}

Buffer::~Buffer()
{
  doca_mmap_destroy(dmap);
  doca_dev_close(dev);
  delete[] buf;
}

void Buffer::send(int sockfd)
{
  const void *export_desc = NULL;
  size_t export_desc_len = 0;
  CHECK_DOCA(doca_mmap_export_pci(dmap, dev, &export_desc, &export_desc_len));

  buffer_desc bd = { .addr = buf, .len = len, .export_desc_len = export_desc_len };
  full_write(sockfd, (char *)&bd, sizeof(bd));
  full_write(sockfd, (char *)export_desc, export_desc_len);
}

// ------------------------------------------------------------------- completion callbacks
static void dma_completed_cb(struct doca_dma_task_memcpy *task,
    union doca_data task_ud, union doca_data ctx_ud)
{
  dma_worker *w = (dma_worker *)ctx_ud.ptr;
  jobspec *job = (jobspec *)task_ud.ptr;
  w->completed.push(*job);
  delete job;

  struct doca_buf *src = (struct doca_buf *)doca_dma_task_memcpy_get_src(task);
  struct doca_buf *dst = doca_dma_task_memcpy_get_dst(task);
  doca_task_free(doca_dma_task_memcpy_as_task(task));
  if (src) doca_buf_dec_refcount(src, NULL);
  if (dst) doca_buf_dec_refcount(dst, NULL);
}

static void dma_error_cb(struct doca_dma_task_memcpy *task,
    union doca_data task_ud, union doca_data ctx_ud)
{
  FAIL("DOCA DMA task failed: " << doca_error_get_descr(
      doca_task_get_status(doca_dma_task_memcpy_as_task(task))));
}

// ------------------------------------------------------------------- Engine (DPU side)
Engine::Engine(unsigned num_clients, unsigned num_workers, int *socks)
  : num_clients(num_clients)
  , num_workers(num_workers)
{
  CHECK(num_clients > 0 && num_workers > 0);
  dev = open_device(dpu_pci());

  auto bds = new buffer_desc[num_clients];
  for (unsigned i = 0; i < num_clients; i++)
    full_read(socks[i], (char *)&bds[i], sizeof(bds[i]));

  buflen = bds[0].len;
  for (unsigned i = 1; i < num_clients; i++)
    CHECK(bds[i].len == buflen);

  local_buf = new char[num_clients * buflen];
  remote_addr = new char*[num_clients];

  CHECK_DOCA(doca_mmap_create(&local_map));
  CHECK_DOCA(doca_mmap_add_dev(local_map, dev));
  CHECK_DOCA(doca_mmap_set_memrange(local_map, local_buf, num_clients * buflen));
  CHECK_DOCA(doca_mmap_start(local_map));

  remote_map = new doca_mmap*[num_clients];
  for (unsigned i = 0; i < num_clients; i++) {
    char *export_desc = new char[bds[i].export_desc_len];
    full_read(socks[i], export_desc, bds[i].export_desc_len);
    CHECK_DOCA(doca_mmap_create_from_export(NULL, export_desc, bds[i].export_desc_len,
          dev, &remote_map[i]));
    delete[] export_desc;
    remote_addr[i] = (char *)bds[i].addr;
  }
  delete[] bds;

  workers = new dma_worker[num_workers];
  for (unsigned w = 0; w < num_workers; w++) {
    CHECK_DOCA(doca_dma_create(dev, &workers[w].dma));
    workers[w].ctx = doca_dma_as_ctx(workers[w].dma);
    CHECK_DOCA(doca_pe_create(&workers[w].pe));
    CHECK_DOCA(doca_pe_connect_ctx(workers[w].pe, workers[w].ctx));
    CHECK_DOCA(doca_dma_task_memcpy_set_conf(workers[w].dma, dma_completed_cb, dma_error_cb, NUM_DMA_TASKS));
    union doca_data ctx_ud = { .ptr = &workers[w] };
    doca_ctx_set_user_data(workers[w].ctx, ctx_ud);
    CHECK_DOCA(doca_buf_inventory_create(2*num_clients + 2, &workers[w].inv));
    CHECK_DOCA(doca_buf_inventory_start(workers[w].inv));
    CHECK_DOCA(doca_ctx_start(workers[w].ctx));
  }
}

Engine::~Engine()
{
  for (unsigned w = 0; w < num_workers; w++) {
    doca_ctx_stop(workers[w].ctx);
    doca_buf_inventory_destroy(workers[w].inv);
    doca_dma_destroy(workers[w].dma);
    doca_pe_destroy(workers[w].pe);
  }
  delete[] workers;

  for (unsigned i = 0; i < num_clients; i++)
    doca_mmap_destroy(remote_map[i]);
  delete[] remote_map;
  delete[] remote_addr;

  doca_mmap_destroy(local_map);
  doca_dev_close(dev);
  delete[] local_buf;
}

void Engine::transfer(unsigned worker, jobspec job)
{
  assert(job.client < num_clients && worker < num_workers);
  assert((size_t)job.offset + job.len <= buflen);

  dma_worker &w = workers[worker];
  char *lptr = local_buf + job.client*buflen + job.offset;
  char *rptr = remote_addr[job.client] + job.offset;

  struct doca_buf *lbuf, *rbuf;
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(w.inv, local_map, lptr, job.len, &lbuf));
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(w.inv, remote_map[job.client], rptr, job.len, &rbuf));

  struct doca_buf *src, *dst;
  if (job.dir == H2D) {                          // pull host -> DPU local
    CHECK_DOCA(doca_buf_set_data(rbuf, rptr, job.len));
    CHECK_DOCA(doca_buf_set_data(lbuf, lptr, 0));
    src = rbuf; dst = lbuf;
  } else {                                        // push DPU local -> host
    CHECK_DOCA(doca_buf_set_data(lbuf, lptr, job.len));
    CHECK_DOCA(doca_buf_set_data(rbuf, rptr, 0));
    src = lbuf; dst = rbuf;
  }

  union doca_data task_ud = { .ptr = new jobspec(job) };
  struct doca_dma_task_memcpy *task;
  CHECK_DOCA(doca_dma_task_memcpy_alloc_init(w.dma, src, dst, task_ud, &task));
  CHECK_DOCA(doca_task_submit(doca_dma_task_memcpy_as_task(task)));
}

bool Engine::poll(unsigned worker, jobspec *job)
{
  dma_worker &w = workers[worker];
  doca_pe_progress(w.pe);
  if (w.completed.empty())
    return false;
  *job = w.completed.front();
  w.completed.pop();
  return true;
}

} // namespace buddy::dma
