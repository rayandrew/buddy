#include <cassert>
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

struct jobdata {
  uint32_t client;
  uint32_t offset:31;
  uint32_t dir:31;
};

static doca_data encode_user_data(unsigned client, uint32_t offset, direction dir)
{
  jobdata jd = { .client = client, .offset = offset, .dir = dir };

  assert(jd.client == client);
  assert(jd.offset == offset);
  assert(jd.dir == dir);

  uint64_t *ptr = reinterpret_cast<uint64_t*>(&jd);
  doca_data dd = { .u64 = *ptr };
  return dd;
}

static void decode_user_data(doca_data data, unsigned *client, uint32_t *offset, direction *dir)
{
  jobdata *jd = reinterpret_cast<jobdata*>(&data.u64);

  *client = jd->client;
  *offset = jd->offset;
  *dir = (direction)jd->dir;
}

void Engine::transfer(unsigned client, uint32_t offset, size_t len, direction dir)
{
  assert(client < num_clients);
  assert(len <= buflen);
  assert(offset < buflen);

  CHECK_DOCA(doca_buf_set_data(doca_buf_local[client], local_buf + client*buflen + offset, len));
  CHECK_DOCA(doca_buf_set_data(doca_buf_remote[client], remote_addr[client] + offset, len));

  doca_buf *src_buf, *dst_buf;
  if (dir == H2D) {
    src_buf = doca_buf_remote[client];
    dst_buf = doca_buf_local[client];
  } else {
    src_buf = doca_buf_local[client];
    dst_buf = doca_buf_remote[client];
  }

  CHECK_DOCA(doca_buf_reset_data_len(dst_buf));

  doca_dma_job_memcpy dma_job = {
    .base = {
      .type = DOCA_DMA_JOB_MEMCPY,
      .flags = DOCA_JOB_FLAGS_NONE,
      .ctx = ctx,
      .user_data = encode_user_data(client, offset, dir),
    },
    .dst_buff = dst_buf,
    .src_buff = src_buf,
  };

  CHECK_DOCA(doca_workq_submit(workq, &dma_job.base));
}

bool Engine::poll(unsigned *client, uint32_t *offset, direction *dir)
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

  decode_user_data(event.user_data, client, offset, dir);
  assert(*client < num_clients);
  assert(*offset < buflen);
  assert(*dir == H2D || *dir == D2H);

  return true;
}

} // namespace buddy::dma
