#include <cassert>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_mmap.h>
#include <doca_buf_inventory.h>

#include "valgrind/memcheck.h"

#include "dma.h"
#include "util.h"
#include "sockets.h"

#define HOST_PCIE "21:00.0"
#define DPU_PCIE  "03:00.0"

namespace buddy::dma {

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
  size_t i;

  CHECK_DOCA(doca_devinfo_list_create(&dev_list, &nb_devs));

  for (i = 0; i < nb_devs; i++) {
    CHECK_DOCA(doca_devinfo_get_is_pci_addr_equal(dev_list[i], pci_addr, &is_addr_equal));
    if (is_addr_equal) {
      if (doca_dma_job_get_supported(dev_list[i], DOCA_DMA_JOB_MEMCPY) != DOCA_SUCCESS)
        continue;

      struct doca_dev *retval = NULL;
      auto res = doca_dev_open(dev_list[i], &retval);
      if (res == DOCA_SUCCESS) {
        doca_devinfo_list_destroy(dev_list);
        return retval;
      }
    }
  }

  FAIL("supported DMA device not found");
}

Buffer::Buffer(size_t len)
  : buf(new char[len])
  , len(len)
{
  dev = open_device(HOST_PCIE);

  CHECK_DOCA(doca_mmap_create(NULL, &dmap));
  CHECK_DOCA(doca_mmap_dev_add(dmap, dev));
  CHECK_DOCA(doca_mmap_set_permissions(dmap, DOCA_ACCESS_DPU_READ_WRITE));
  CHECK_DOCA(doca_mmap_set_memrange(dmap, buf, len));
  CHECK_DOCA(doca_mmap_start(dmap));
}

void Buffer::send(int sockfd)
{
  const void *export_desc = NULL;
  size_t export_desc_len = 0;
  CHECK_DOCA(doca_mmap_export_dpu(dmap, dev, &export_desc, &export_desc_len));

  buffer_desc bd = {
    .addr = buf,
    .len = len,
    .export_desc_len = export_desc_len,
  };

  full_write(sockfd, (char *)&bd, sizeof(bd));
  full_write(sockfd, (char *)export_desc, export_desc_len);
}

Engine::Engine(unsigned num_clients, int *socks)
  : num_clients(num_clients)
{
  CHECK(num_clients > 0);

  std::cout << "Setting up DMA engine... " << std::flush;

  dev = open_device(DPU_PCIE);

  auto bds = new buffer_desc[num_clients];

  for (unsigned i = 0; i < num_clients; i++)
    full_read(socks[i], (char*)&bds[i], sizeof(bds[i]));

  buflen = bds[0].len;
  for (unsigned i = 1; i < num_clients; i++)
    CHECK(bds[i].len == buflen);

  local_buf = new char[num_clients * buflen];
  remote_addr = new char*[num_clients];

  doca_dma *dma_ctx;
  CHECK_DOCA(doca_dma_create(&dma_ctx));
  ctx = doca_dma_as_ctx(dma_ctx);

  CHECK_DOCA(doca_mmap_create(NULL, &local_map));
  CHECK_DOCA(doca_mmap_dev_add(local_map, dev));

  CHECK_DOCA(doca_buf_inventory_create(NULL, 2*num_clients,
        DOCA_BUF_EXTENSION_NONE, &buf_inv));
  CHECK_DOCA(doca_buf_inventory_start(buf_inv));

  CHECK_DOCA(doca_ctx_dev_add(ctx, dev));
  CHECK_DOCA(doca_ctx_start(ctx));

  CHECK_DOCA(doca_workq_create(num_clients, &workq));
  CHECK_DOCA(doca_ctx_workq_add(ctx, workq));

  CHECK_DOCA(doca_mmap_set_memrange(local_map, local_buf,
        num_clients * buflen));
  CHECK_DOCA(doca_mmap_start(local_map));

  remote_map = new doca_mmap*[num_clients];
  doca_buf_local = new doca_buf*[num_clients];
  doca_buf_remote = new doca_buf*[num_clients];

  for (unsigned i = 0; i < num_clients; i++) {
    char *export_desc = new char[bds[i].export_desc_len];
    full_read(socks[i], export_desc, bds[i].export_desc_len);

    CHECK_DOCA(doca_mmap_create_from_export(NULL, export_desc,
          bds[i].export_desc_len, dev, &remote_map[i]));
    delete[] export_desc;
  }

  for (unsigned i = 0; i < num_clients; i++) {
    char *lbuf = local_buf + i*buflen;
    CHECK_DOCA(doca_buf_inventory_buf_by_addr(buf_inv, local_map,
          lbuf, buflen, &doca_buf_local[i]));

    CHECK_DOCA(doca_buf_inventory_buf_by_addr(buf_inv, remote_map[i],
          bds[i].addr, buflen, &doca_buf_remote[i]));
    remote_addr[i] = (char *)bds[i].addr;
  }

  delete[] bds;

  std::cout << "ok" << std::endl;
}

void Engine::transfer(jobspec job)
{
  assert(job.client < num_clients);
  assert(job.len <= buflen);
  assert(job.offset < buflen);

  CHECK_DOCA(doca_buf_set_data(doca_buf_local[job.client],
        local_buf + job.client*buflen + job.offset, job.len));
  CHECK_DOCA(doca_buf_set_data(doca_buf_remote[job.client],
        remote_addr[job.client] + job.offset, job.len));

  doca_buf *src_buf, *dst_buf;
  if (job.dir == H2D) {
    src_buf = doca_buf_remote[job.client];
    dst_buf = doca_buf_local[job.client];

    VALGRIND_MAKE_MEM_UNDEFINED(local_buf + job.client*buflen + job.offset, job.len);
  } else {
    src_buf = doca_buf_local[job.client];
    dst_buf = doca_buf_remote[job.client];

    VALGRIND_CHECK_MEM_IS_DEFINED(
        local_buf + job.client*buflen + job.offset, job.len);
  }

  CHECK_DOCA(doca_buf_reset_data_len(dst_buf));

  auto jobptr = new jobspec(job);

  doca_dma_job_memcpy dma_job = {
    .base = {
      .type = DOCA_DMA_JOB_MEMCPY,
      .flags = DOCA_JOB_FLAGS_NONE,
      .ctx = ctx,
      .user_data = { .ptr = jobptr },
    },
    .dst_buff = dst_buf,
    .src_buff = src_buf,
  };

  CHECK_DOCA(doca_workq_submit(workq, &dma_job.base));
}

bool Engine::poll(jobspec *job)
{
  doca_event event = {0};
  doca_error_t result;
  result = doca_workq_progress_retrieve(workq, &event, DOCA_WORKQ_RETRIEVE_FLAGS_NONE);
  
  if (result == DOCA_ERROR_AGAIN) {
    return false;
  }/* else if(result == DOCA_ERROR_IO_FAILED) {
    result = (doca_error_t)event.result.u64;
  }*/
  CHECK_DOCA(result);
  CHECK_DOCA((doca_error_t)event.result.u64);

  jobspec *jobptr = (jobspec*)event.user_data.ptr;
  assert(jobptr->client < num_clients);
  assert(jobptr->offset < buflen);
  assert(jobptr->dir == H2D || jobptr->dir == D2H);

  if (jobptr->dir == H2D)
    VALGRIND_MAKE_MEM_DEFINED(
        local_buf + jobptr->client*buflen + jobptr->offset, jobptr->len);

  *job = *jobptr;
  delete jobptr;

  return true;
}

} // namespace buddy::dma
