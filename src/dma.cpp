#include <cassert>
#include <cstdlib>
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
  Engine *engine = (Engine *)ctx_ud.ptr;
  jobspec *job = (jobspec *)task_ud.ptr;
  engine->on_complete(*job);
  delete job;
  doca_task_free(doca_dma_task_memcpy_as_task(task));   // bufs are persistent -> don't free them
}

static void dma_error_cb(struct doca_dma_task_memcpy *task,
    union doca_data task_ud, union doca_data ctx_ud)
{
  FAIL("DOCA DMA task failed: " << doca_error_get_descr(
      doca_task_get_status(doca_dma_task_memcpy_as_task(task))));
}

// ------------------------------------------------------------------- Engine (DPU side)
Engine::Engine(unsigned num_clients, int *socks)
  : num_clients(num_clients)
{
  CHECK(num_clients > 0);
  dev = open_device(dpu_pci());

  auto bds = new buffer_desc[num_clients];
  for (unsigned i = 0; i < num_clients; i++)
    full_read(socks[i], (char *)&bds[i], sizeof(bds[i]));

  buflen = bds[0].len;
  for (unsigned i = 1; i < num_clients; i++)
    CHECK(bds[i].len == buflen);

  local_buf = new char[num_clients * buflen];
  remote_addr = new char*[num_clients];

  CHECK_DOCA(doca_dma_create(dev, &dma_ctx));
  ctx = doca_dma_as_ctx(dma_ctx);

  CHECK_DOCA(doca_pe_create(&pe));
  CHECK_DOCA(doca_pe_connect_ctx(pe, ctx));
  CHECK_DOCA(doca_dma_task_memcpy_set_conf(dma_ctx, dma_completed_cb, dma_error_cb, NUM_DMA_TASKS));

  union doca_data ctx_ud = { .ptr = this };
  doca_ctx_set_user_data(ctx, ctx_ud);

  CHECK_DOCA(doca_buf_inventory_create(2*num_clients, &buf_inv));
  CHECK_DOCA(doca_buf_inventory_start(buf_inv));

  CHECK_DOCA(doca_mmap_create(&local_map));
  CHECK_DOCA(doca_mmap_add_dev(local_map, dev));
  CHECK_DOCA(doca_mmap_set_memrange(local_map, local_buf, num_clients * buflen));
  CHECK_DOCA(doca_mmap_start(local_map));

  CHECK_DOCA(doca_ctx_start(ctx));

  remote_map = new doca_mmap*[num_clients];
  doca_buf_local = new doca_buf*[num_clients];
  doca_buf_remote = new doca_buf*[num_clients];

  for (unsigned i = 0; i < num_clients; i++) {
    char *export_desc = new char[bds[i].export_desc_len];
    full_read(socks[i], export_desc, bds[i].export_desc_len);
    CHECK_DOCA(doca_mmap_create_from_export(NULL, export_desc, bds[i].export_desc_len,
          dev, &remote_map[i]));
    delete[] export_desc;

    char *lbuf = local_buf + i*buflen;
    CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(buf_inv, local_map, lbuf, buflen, &doca_buf_local[i]));
    CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(buf_inv, remote_map[i], bds[i].addr, buflen, &doca_buf_remote[i]));
    remote_addr[i] = (char *)bds[i].addr;
  }

  delete[] bds;
}

Engine::~Engine()
{
  for (unsigned i = 0; i < num_clients; i++) {
    doca_buf_dec_refcount(doca_buf_local[i], NULL);
    doca_buf_dec_refcount(doca_buf_remote[i], NULL);
    doca_mmap_destroy(remote_map[i]);
  }
  delete[] doca_buf_remote;
  delete[] doca_buf_local;
  delete[] remote_map;
  delete[] remote_addr;

  doca_ctx_stop(ctx);
  doca_buf_inventory_destroy(buf_inv);
  doca_mmap_destroy(local_map);
  doca_dma_destroy(dma_ctx);
  doca_pe_destroy(pe);
  doca_dev_close(dev);
  delete[] local_buf;
}

void Engine::transfer(jobspec job)
{
  assert(job.client < num_clients);
  assert((size_t)job.offset + job.len <= buflen);

  char *lptr = local_buf + job.client*buflen + job.offset;
  char *rptr = remote_addr[job.client] + job.offset;

  doca_buf *src_buf, *dst_buf;
  if (job.dir == H2D) {                       // pull host -> DPU local
    CHECK_DOCA(doca_buf_set_data(doca_buf_remote[job.client], rptr, job.len));
    CHECK_DOCA(doca_buf_set_data(doca_buf_local[job.client], lptr, 0));
    src_buf = doca_buf_remote[job.client];
    dst_buf = doca_buf_local[job.client];
  } else {                                     // push DPU local -> host
    CHECK_DOCA(doca_buf_set_data(doca_buf_local[job.client], lptr, job.len));
    CHECK_DOCA(doca_buf_set_data(doca_buf_remote[job.client], rptr, 0));
    src_buf = doca_buf_local[job.client];
    dst_buf = doca_buf_remote[job.client];
  }

  union doca_data task_ud = { .ptr = new jobspec(job) };
  struct doca_dma_task_memcpy *task;
  CHECK_DOCA(doca_dma_task_memcpy_alloc_init(dma_ctx, src_buf, dst_buf, task_ud, &task));
  CHECK_DOCA(doca_task_submit(doca_dma_task_memcpy_as_task(task)));
}

bool Engine::poll(jobspec *job)
{
  doca_pe_progress(pe);            // drives completion callbacks -> `completed`
  if (completed.empty())
    return false;
  *job = completed.front();
  completed.pop();
  return true;
}

} // namespace buddy::dma
