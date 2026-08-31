#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <poll.h>

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

static double now_seconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The task pools, the buf inventory and the send queue are all finite. DOCA reports exhaustion of
// each as a retryable status, not a failure: draining the progress engine releases all three. The
// deadline turns a genuinely wedged queue into a diagnosable error instead of a hang.
static const double kDrainTimeout = 60.0;

static bool drain_and_retry(struct doca_pe *pe, doca_error_t err, double *deadline)
{
  if (err != DOCA_ERROR_FULL && err != DOCA_ERROR_NO_MEMORY && err != DOCA_ERROR_AGAIN) return false;
  const double now = now_seconds();
  if (*deadline == 0.0) *deadline = now + kDrainTimeout;
  else if (now > *deadline) return false;
  doca_pe_progress(pe);
  return true;
}

// Submit-side counterpart to CHECK_DOCA: retries the transient statuses above instead of aborting.
#define DOCA_SUBMIT(pe, a) do {                                                       \
    doca_error_t _err; double _dl = 0.0;                                              \
    while ((_err = (a)) != DOCA_SUCCESS)                                              \
      if (!drain_and_retry(pe, _err, &_dl))                                           \
        FAIL("doca " << doca_error_get_descr(_err));                                  \
  } while (0)

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

static void conn_established_cb(struct doca_rdma_connection *, union doca_data, union doca_data cu)
{ ((DocaRdma *)cu.ptr)->on_established(); }
static void conn_failure_cb(struct doca_rdma_connection *, union doca_data, union doca_data cu)
{ ((DocaRdma *)cu.ptr)->on_failed(); }
static void conn_disconnect_cb(struct doca_rdma_connection *, union doca_data, union doca_data cu)
{ ((DocaRdma *)cu.ptr)->on_failed(); }

static void require_task(doca_error_t rc, const char *what)
{ if (rc != DOCA_SUCCESS) FAIL("doca rdma " << what << " unsupported here: " << doca_error_get_descr(rc)); }

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

  const struct doca_devinfo *info = doca_dev_as_devinfo(dev);
  require_task(doca_rdma_cap_task_send_imm_is_supported(info), "send_imm");
  require_task(doca_rdma_cap_task_receive_is_supported(info), "receive");
  require_task(doca_rdma_cap_task_read_is_supported(info), "read");
  require_task(doca_rdma_cap_task_write_is_supported(info), "write");
  require_task(doca_rdma_cap_task_write_imm_is_supported(info), "write_imm");

  // Size the send queue to the task pool. Left unset it takes a library default that can sit far
  // below NUM_RDMA_TASKS, so a burst returns DOCA_ERROR_FULL as a matter of course.
  uint32_t max_sq = 0, max_rq = 0;
  CHECK_DOCA(doca_rdma_cap_get_max_send_queue_size(info, &max_sq));
  CHECK_DOCA(doca_rdma_cap_get_max_recv_queue_size(info, &max_rq));
  CHECK_DOCA(doca_rdma_set_send_queue_size(rdma, std::min<uint32_t>(NUM_RDMA_TASKS, max_sq)));
  // mlx5_2 here reports a recv-queue maximum but rejects every value, one included. Sizing it is
  // an optimization, so take it when the device allows and keep the default when it does not.
  const doca_error_t rq_rc = doca_rdma_set_recv_queue_size(rdma, std::min<uint32_t>(NUM_RDMA_TASKS, max_rq));
  if (rq_rc != DOCA_SUCCESS && rq_rc != DOCA_ERROR_NOT_SUPPORTED)
    FAIL("doca " << doca_error_get_descr(rq_rc));

  CHECK_DOCA(doca_rdma_set_connection_state_callbacks(rdma, NULL, conn_established_cb,
                                                      conn_failure_cb, conn_disconnect_cb));

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
  // Stop is asynchronous while tasks are still in flight; the engine has to be driven until the
  // ctx reports idle or the destroys below fail with DOCA_ERROR_IN_USE.
  doca_ctx_stop(ctx);
  enum doca_ctx_states cs;
  const double deadline = now_seconds() + kDrainTimeout;
  do { doca_pe_progress(pe); doca_ctx_get_state(ctx, &cs); }
  while (cs != DOCA_CTX_STATE_IDLE && now_seconds() < deadline);
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

void DocaRdma::wait_connected(double timeout_s)
{
  // The established callback never fires on the export/connect path (it belongs to the RDMA CM
  // flow), so a RUNNING ctx plus the caller's own barrier is all the readiness signal there is.
  // The timeout only keeps a peer that never connects from hanging here forever.
  enum doca_ctx_states cs;
  const double deadline = now_seconds() + timeout_s;
  for (;;) {
    doca_pe_progress(pe);
    doca_ctx_get_state(ctx, &cs);
    if (failed) FAIL("doca rdma connection failed before it was established");
    if (cs == DOCA_CTX_STATE_RUNNING) return;
    if (now_seconds() > deadline) FAIL("doca rdma ctx did not reach RUNNING in " << timeout_s << " s");
  }
}

void DocaRdma::send_imm(unsigned conn_idx, uint32_t imm, size_t offset, size_t len, uint64_t wr_id)
{
  struct doca_buf *src;
  DOCA_SUBMIT(pe, doca_buf_inventory_buf_get_by_data(inv, mmap, mem + offset, len, &src));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_send_imm *task;
  DOCA_SUBMIT(pe, doca_rdma_task_send_imm_allocate_init(rdma, conns[conn_idx], src, htobe32(imm), tu, &task));
  DOCA_SUBMIT(pe, doca_task_submit(doca_rdma_task_send_imm_as_task(task)));
}

void DocaRdma::post_recv(size_t offset, size_t len, uint64_t wr_id)
{
  struct doca_buf *dst;
  DOCA_SUBMIT(pe, doca_buf_inventory_buf_get_by_addr(inv, mmap, mem + offset, len, &dst));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_receive *task;
  DOCA_SUBMIT(pe, doca_rdma_task_receive_allocate_init(rdma, dst, tu, &task));
  DOCA_SUBMIT(pe, doca_task_submit(doca_rdma_task_receive_as_task(task)));
}

void DocaRdma::read(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id)
{
  struct doca_buf *src, *dst;                 // src = peer memory, dst = our memory
  DOCA_SUBMIT(pe, doca_buf_inventory_buf_get_by_addr(inv, remote_mmap[conn_idx], remote_base[conn_idx]+remote_off, len, &src));
  DOCA_SUBMIT(pe, doca_buf_set_data(src, remote_base[conn_idx]+remote_off, len));
  DOCA_SUBMIT(pe, doca_buf_inventory_buf_get_by_addr(inv, mmap, mem+local_off, len, &dst));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_read *task;
  DOCA_SUBMIT(pe, doca_rdma_task_read_allocate_init(rdma, conns[conn_idx], src, dst, tu, &task));
  DOCA_SUBMIT(pe, doca_task_submit(doca_rdma_task_read_as_task(task)));
}

void DocaRdma::write(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint64_t wr_id)
{
  struct doca_buf *src, *dst;                 // src = our memory, dst = peer memory
  DOCA_SUBMIT(pe, doca_buf_inventory_buf_get_by_addr(inv, mmap, mem+local_off, len, &src));
  DOCA_SUBMIT(pe, doca_buf_set_data(src, mem+local_off, len));
  DOCA_SUBMIT(pe, doca_buf_inventory_buf_get_by_addr(inv, remote_mmap[conn_idx], remote_base[conn_idx]+remote_off, len, &dst));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_write *task;
  DOCA_SUBMIT(pe, doca_rdma_task_write_allocate_init(rdma, conns[conn_idx], src, dst, tu, &task));
  DOCA_SUBMIT(pe, doca_task_submit(doca_rdma_task_write_as_task(task)));
}

void DocaRdma::write_imm(unsigned conn_idx, size_t local_off, size_t remote_off, size_t len, uint32_t imm, uint64_t wr_id)
{
  struct doca_buf *src, *dst;
  DOCA_SUBMIT(pe, doca_buf_inventory_buf_get_by_addr(inv, mmap, mem+local_off, len, &src));
  DOCA_SUBMIT(pe, doca_buf_set_data(src, mem+local_off, len));
  DOCA_SUBMIT(pe, doca_buf_inventory_buf_get_by_addr(inv, remote_mmap[conn_idx], remote_base[conn_idx]+remote_off, len, &dst));
  union doca_data tu; tu.u64 = wr_id;
  struct doca_rdma_task_write_imm *task;
  DOCA_SUBMIT(pe, doca_rdma_task_write_imm_allocate_init(rdma, conns[conn_idx], src, dst, htobe32(imm), tu, &task));
  DOCA_SUBMIT(pe, doca_task_submit(doca_rdma_task_write_imm_as_task(task)));
}

bool DocaRdma::poll(completion *c)
{
  // Only touch the hardware when the software queue is dry. A drain loop calls this once per
  // completion, and progressing on each of those repolls the CQ for completions already in hand.
  if (completed.empty()) doca_pe_progress(pe);
  if (completed.empty()) return false;
  *c = completed.front(); completed.pop();
  return true;
}

bool DocaRdma::wait_idle(double timeout_s)
{
  if (!completed.empty()) return true;
  doca_notification_handle_t handle;
  if (doca_pe_get_notification_handle(pe, &handle) != DOCA_SUCCESS) return false;
  if (doca_pe_request_notification(pe) != DOCA_SUCCESS) return false;

  struct pollfd pfd = { handle, POLLIN, 0 };
  struct timespec ts;
  ts.tv_sec = (time_t)timeout_s;
  ts.tv_nsec = (long)((timeout_s - (double)ts.tv_sec) * 1e9);
  ppoll(&pfd, 1, &ts, NULL);

  doca_pe_clear_notification(pe, handle);
  while (doca_pe_progress(pe)) {}
  return !completed.empty();
}

unsigned DocaRdma::conn_index_of(const struct doca_rdma_connection *conn)
{
  for (unsigned i = 0; i < num_connections; i++)
    if (conns[i] == conn) return i;
  FAIL("unknown doca_rdma connection");
}

} // namespace buddy::rdma
