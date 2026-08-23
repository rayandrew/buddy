#include <cstdlib>
#include <cstring>
#include <endian.h>

#include <doca_dev.h>
#include <doca_rdma.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_mmap.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_error.h>

#include "rdma_doca.h"
#include "sockets.h"
#include "util.h"

#define NUM_RDMA_TASKS 4096

namespace buddy::rdma {

static const char *rdma_ibdev() { const char *e = getenv("BUDDY_RDMA_DEV"); return e && *e ? e : "mlx5_2"; }
static uint32_t rdma_gid()      { const char *e = getenv("BUDDY_GID_INDEX"); return e && *e ? atoi(e) : 0; }

static struct doca_dev *open_rdma_dev(const char *ibdev)
{
  struct doca_devinfo **list; uint32_t n = 0;
  char name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
  CHECK_DOCA(doca_devinfo_create_list(&list, &n));
  for (uint32_t i = 0; i < n; i++) {
    if (doca_devinfo_get_ibdev_name(list[i], name, sizeof(name)) != DOCA_SUCCESS) continue;
    if (strncmp(ibdev, name, sizeof(name)) != 0) continue;
    struct doca_dev *dev = NULL;
    if (doca_dev_open(list[i], &dev) == DOCA_SUCCESS) { doca_devinfo_destroy_list(list); return dev; }
  }
  doca_devinfo_destroy_list(list);
  FAIL("no doca rdma device ibdev=" << ibdev);
}

static void free_two(struct doca_buf *a, struct doca_buf *b)
{ if (a) doca_buf_dec_refcount(a, NULL); if (b) doca_buf_dec_refcount(b, NULL); }

static void send_cb(struct doca_rdma_task_send_imm *task, union doca_data tu, union doca_data cu)
{
  DocaRdma *self = (DocaRdma *)cu.ptr;
  self->push({ tu.u64, 0, 0, 0, DocaRdma::OP_SEND });
  struct doca_buf *src = (struct doca_buf *)doca_rdma_task_send_imm_get_src_buf(task);
  doca_task_free(doca_rdma_task_send_imm_as_task(task));
  free_two(src, NULL);
}
static void send_err(struct doca_rdma_task_send_imm *task, union doca_data, union doca_data)
{ FAIL("doca_rdma send failed: " << doca_error_get_descr(doca_task_get_status(doca_rdma_task_send_imm_as_task(task)))); }

static void recv_cb(struct doca_rdma_task_receive *task, union doca_data tu, union doca_data cu)
{
  DocaRdma *self = (DocaRdma *)cu.ptr;
  uint32_t imm = be32toh(doca_rdma_task_receive_get_result_immediate_data(task));
  uint32_t len = doca_rdma_task_receive_get_result_len(task);
  unsigned conn = self->conn_index_of(doca_rdma_task_receive_get_result_rdma_connection(task));
  self->push({ tu.u64, imm, len, conn, DocaRdma::OP_RECV });
  struct doca_buf *dst = doca_rdma_task_receive_get_dst_buf(task);
  doca_task_free(doca_rdma_task_receive_as_task(task));
  free_two(dst, NULL);
}
static void recv_err(struct doca_rdma_task_receive *task, union doca_data, union doca_data)
{ FAIL("doca_rdma recv failed: " << doca_error_get_descr(doca_task_get_status(doca_rdma_task_receive_as_task(task)))); }

static void read_cb(struct doca_rdma_task_read *task, union doca_data tu, union doca_data cu)
{
  DocaRdma *self = (DocaRdma *)cu.ptr;
  self->push({ tu.u64, 0, doca_rdma_task_read_get_result_len(task), 0, DocaRdma::OP_READ });
  free_two((struct doca_buf *)doca_rdma_task_read_get_src_buf(task), doca_rdma_task_read_get_dst_buf(task));
  doca_task_free(doca_rdma_task_read_as_task(task));
}
static void read_err(struct doca_rdma_task_read *task, union doca_data, union doca_data)
{ FAIL("doca_rdma read failed: " << doca_error_get_descr(doca_task_get_status(doca_rdma_task_read_as_task(task)))); }

static void write_cb(struct doca_rdma_task_write *task, union doca_data tu, union doca_data cu)
{
  DocaRdma *self = (DocaRdma *)cu.ptr;
  self->push({ tu.u64, 0, 0, 0, DocaRdma::OP_WRITE });
  free_two((struct doca_buf *)doca_rdma_task_write_get_src_buf(task), doca_rdma_task_write_get_dst_buf(task));
  doca_task_free(doca_rdma_task_write_as_task(task));
}
static void write_err(struct doca_rdma_task_write *task, union doca_data, union doca_data)
{ FAIL("doca_rdma write failed: " << doca_error_get_descr(doca_task_get_status(doca_rdma_task_write_as_task(task)))); }

static void write_imm_cb(struct doca_rdma_task_write_imm *task, union doca_data tu, union doca_data cu)
{
  DocaRdma *self = (DocaRdma *)cu.ptr;
  self->push({ tu.u64, 0, 0, 0, DocaRdma::OP_WRITE });
  free_two((struct doca_buf *)doca_rdma_task_write_imm_get_src_buf(task), doca_rdma_task_write_imm_get_dst_buf(task));
  doca_task_free(doca_rdma_task_write_imm_as_task(task));
}
static void write_imm_err(struct doca_rdma_task_write_imm *task, union doca_data, union doca_data)
{ FAIL("doca_rdma write_imm failed: " << doca_error_get_descr(doca_task_get_status(doca_rdma_task_write_imm_as_task(task)))); }

DocaRdma::DocaRdma(unsigned num_connections, char *mem, size_t mem_len)
  : num_connections(num_connections)
  , mem(mem)
  , mem_len(mem_len)
{
  const uint32_t perms = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE
      | DOCA_ACCESS_FLAG_RDMA_READ | DOCA_ACCESS_FLAG_RDMA_WRITE;   // peers may read/write our mem
  dev = open_rdma_dev(rdma_ibdev());
  CHECK_DOCA(doca_rdma_create(dev, &rdma));
  ctx = doca_rdma_as_ctx(rdma);
  CHECK_DOCA(doca_rdma_set_permissions(rdma, perms));
  CHECK_DOCA(doca_rdma_set_grh_enabled(rdma, 1));
  CHECK_DOCA(doca_rdma_set_gid_index(rdma, rdma_gid()));
  CHECK_DOCA(doca_rdma_set_max_num_connections(rdma, num_connections));
  // Match the ibverbs leg's rnr_retry=7 (infinite); the DOCA default is finite.
  CHECK_DOCA(doca_rdma_set_rnr_retry_count(rdma, 7));

  CHECK_DOCA(doca_pe_create(&pe));
  CHECK_DOCA(doca_pe_connect_ctx(pe, ctx));
  CHECK_DOCA(doca_rdma_task_send_imm_set_conf(rdma, send_cb, send_err, NUM_RDMA_TASKS));
  CHECK_DOCA(doca_rdma_task_receive_set_conf(rdma, recv_cb, recv_err, NUM_RDMA_TASKS));
  CHECK_DOCA(doca_rdma_task_read_set_conf(rdma, read_cb, read_err, NUM_RDMA_TASKS));
  CHECK_DOCA(doca_rdma_task_write_set_conf(rdma, write_cb, write_err, NUM_RDMA_TASKS));
  CHECK_DOCA(doca_rdma_task_write_imm_set_conf(rdma, write_imm_cb, write_imm_err, NUM_RDMA_TASKS));
  union doca_data cu = { .ptr = this };
  doca_ctx_set_user_data(ctx, cu);

  CHECK_DOCA(doca_mmap_create(&mmap));
  CHECK_DOCA(doca_mmap_add_dev(mmap, dev));
  CHECK_DOCA(doca_mmap_set_permissions(mmap, perms));
  CHECK_DOCA(doca_mmap_set_memrange(mmap, mem, mem_len));
  CHECK_DOCA(doca_mmap_start(mmap));
  CHECK_DOCA(doca_buf_inventory_create(2*NUM_RDMA_TASKS, &inv));
  CHECK_DOCA(doca_buf_inventory_start(inv));

  CHECK_DOCA(doca_ctx_start(ctx));
  conns = new doca_rdma_connection*[num_connections]();
  remote_mmap = new doca_mmap*[num_connections]();
  remote_base = new char*[num_connections]();
}

DocaRdma::~DocaRdma()
{
  doca_ctx_stop(ctx);
  doca_buf_inventory_destroy(inv);
  doca_mmap_destroy(mmap);
  doca_rdma_destroy(rdma);
  doca_pe_destroy(pe);
  for (unsigned i = 0; i < num_connections; i++)
    if (remote_mmap[i]) doca_mmap_destroy(remote_mmap[i]);
  doca_dev_close(dev);
  delete[] conns;
  delete[] remote_mmap;
  delete[] remote_base;
}

void DocaRdma::connect(unsigned idx, int sock, bool is_server)
{
  const void *blob; size_t blob_len;
  CHECK_DOCA(doca_rdma_export(rdma, &blob, &blob_len, &conns[idx]));

  uint64_t bl = blob_len;
  full_write(sock, (char *)&bl, sizeof(bl));
  full_write(sock, (char *)blob, blob_len);
  uint64_t pbl; full_read(sock, (char *)&pbl, sizeof(pbl));
  char *pblob = new char[pbl]; full_read(sock, pblob, pbl);
  CHECK_DOCA(doca_rdma_connect(rdma, pblob, pbl, conns[idx]));
  delete[] pblob;

  // also exchange the mmap export so read/write can reach the peer's memory
  const void *mdesc; size_t mdesc_len;
  CHECK_DOCA(doca_mmap_export_rdma(mmap, dev, &mdesc, &mdesc_len));
  uint64_t ml = mdesc_len;
  full_write(sock, (char *)&ml, sizeof(ml));
  full_write(sock, (char *)mdesc, mdesc_len);
  uint64_t pml; full_read(sock, (char *)&pml, sizeof(pml));
  char *pmdesc = new char[pml]; full_read(sock, pmdesc, pml);
  CHECK_DOCA(doca_mmap_create_from_export(NULL, pmdesc, pml, dev, &remote_mmap[idx]));
  delete[] pmdesc;
  size_t rlen;
  CHECK_DOCA(doca_mmap_get_memrange(remote_mmap[idx], (void **)&remote_base[idx], &rlen));
  (void)is_server;
}

void DocaRdma::wait_connected()
{
  enum doca_ctx_states cs;
  do { doca_pe_progress(pe); doca_ctx_get_state(ctx, &cs); } while (cs != DOCA_CTX_STATE_RUNNING);
}

void DocaRdma::send_imm(unsigned conn_idx, uint32_t imm, size_t offset, size_t len, uint64_t wr_id)
{
  struct doca_buf *src;
  CHECK_DOCA(doca_buf_inventory_buf_get_by_data(inv, mmap, mem + offset, len, &src));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_send_imm *task;
  CHECK_DOCA(doca_rdma_task_send_imm_allocate_init(rdma, conns[conn_idx], src, htobe32(imm), tu, &task));
  CHECK_DOCA(doca_task_submit(doca_rdma_task_send_imm_as_task(task)));
}

void DocaRdma::post_recv(size_t offset, size_t len, uint64_t wr_id)
{
  struct doca_buf *dst;
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, mmap, mem + offset, len, &dst));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_receive *task;
  CHECK_DOCA(doca_rdma_task_receive_allocate_init(rdma, dst, tu, &task));
  CHECK_DOCA(doca_task_submit(doca_rdma_task_receive_as_task(task)));
}

void DocaRdma::read(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id)
{
  struct doca_buf *src, *dst;                 // src = peer memory, dst = our memory
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, remote_mmap[conn_idx], remote_base[conn_idx]+remote_off, len, &src));
  CHECK_DOCA(doca_buf_set_data(src, remote_base[conn_idx]+remote_off, len));
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, mmap, mem+local_off, len, &dst));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_read *task;
  CHECK_DOCA(doca_rdma_task_read_allocate_init(rdma, conns[conn_idx], src, dst, tu, &task));
  CHECK_DOCA(doca_task_submit(doca_rdma_task_read_as_task(task)));
}

void DocaRdma::write(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id)
{
  struct doca_buf *src, *dst;                 // src = our memory, dst = peer memory
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, mmap, mem+local_off, len, &src));
  CHECK_DOCA(doca_buf_set_data(src, mem+local_off, len));
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, remote_mmap[conn_idx], remote_base[conn_idx]+remote_off, len, &dst));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_write *task;
  CHECK_DOCA(doca_rdma_task_write_allocate_init(rdma, conns[conn_idx], src, dst, tu, &task));
  CHECK_DOCA(doca_task_submit(doca_rdma_task_write_as_task(task)));
}

void DocaRdma::write_imm(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint32_t imm, uint64_t wr_id)
{
  struct doca_buf *src, *dst;
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, mmap, mem+local_off, len, &src));
  CHECK_DOCA(doca_buf_set_data(src, mem+local_off, len));
  CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, remote_mmap[conn_idx], remote_base[conn_idx]+remote_off, len, &dst));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_write_imm *task;
  CHECK_DOCA(doca_rdma_task_write_imm_allocate_init(rdma, conns[conn_idx], src, dst, htobe32(imm), tu, &task));
  CHECK_DOCA(doca_task_submit(doca_rdma_task_write_imm_as_task(task)));
}

bool DocaRdma::poll(completion *c)
{
  doca_pe_progress(pe);
  if (completed.empty()) return false;
  *c = completed.front(); completed.pop();
  return true;
}

unsigned DocaRdma::conn_index_of(const struct doca_rdma_connection *conn)
{
  for (unsigned i = 0; i < num_connections; i++)
    if (conns[i] == conn) return i;
  FAIL("unknown doca_rdma connection");
}

} // namespace buddy::rdma
