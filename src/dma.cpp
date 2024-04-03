#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_mmap.h>
#include <doca_buf_inventory.h>

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

struct doca_dev *open_device(const char *pci_addr)
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
  }

  delete[] bds;

  std::cout << "ok" << std::endl;
}

} // namespace buddy::dma
