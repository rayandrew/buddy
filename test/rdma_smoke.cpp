// Minimal DPU<->DPU doca_rdma smoke test (DOCA 3.0). Connection details are exchanged over TCP
// (like Buddy's proxy_main), then the client sends one buffer and the server receives + verifies.
// De-risks the doca_rdma connection state machine before wiring it into the proxy D2D path.
//   server: ./rdma_smoke server <port>
//   client: ./rdma_smoke client <server_ip> <port>
// RoCE: use the SF device that has GIDs (BUDDY_RDMA_DEV=mlx5_2, BUDDY_GID_INDEX=0) -- mlx5_0
// has no GIDs on this DPU. Verified sm7-bf<->sm8-bf over OOB control + port-1 RoCE data.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>

#include <doca_dev.h>
#include <doca_rdma.h>
#include <doca_ctx.h>
#include <doca_pe.h>
#include <doca_mmap.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_error.h>

#include "sockets.h"
#include "util.h"

using namespace buddy;

static const size_t LEN = 4096;
static const uint32_t NUM_TASKS = 16;

struct sstate { int remaining = 0; bool ok = false; size_t rlen = 0; };

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
  FAIL("no doca device ibdev=" << ibdev);
}

static void send_cb(struct doca_rdma_task_send *t, union doca_data, union doca_data cu)
{ auto *s = (sstate *)cu.ptr; s->remaining--; s->ok = true; doca_task_free(doca_rdma_task_send_as_task(t)); }
static void send_err(struct doca_rdma_task_send *t, union doca_data, union doca_data cu)
{ auto *s = (sstate *)cu.ptr; s->remaining--; FAIL("send task error"); (void)s; (void)t; }
static void recv_cb(struct doca_rdma_task_receive *t, union doca_data, union doca_data cu)
{ auto *s = (sstate *)cu.ptr; s->remaining--; s->ok = true; s->rlen = doca_rdma_task_receive_get_result_len(t);
  doca_task_free(doca_rdma_task_receive_as_task(t)); }
static void recv_err(struct doca_rdma_task_receive *t, union doca_data, union doca_data cu)
{ auto *s = (sstate *)cu.ptr; s->remaining--; FAIL("recv task error"); (void)s; (void)t; }

int main(int argc, char **argv)
{
  if (argc < 3) { std::cerr << "usage: server <port> | client <ip> <port>\n"; return 2; }
  bool server = !strcmp(argv[1], "server");
  const char *ibdev = getenv("BUDDY_RDMA_DEV"); if (!ibdev || !*ibdev) ibdev = "mlx5_0";

  sstate st;
  char *data = new char[LEN];
  memset(data, server ? 0 : 0xC5, LEN);            // client fills 0xC5, server starts empty

  struct doca_dev *dev = open_rdma_dev(ibdev);
  struct doca_rdma *rdma;
  CHECK_DOCA(doca_rdma_create(dev, &rdma));
  struct doca_ctx *ctx = doca_rdma_as_ctx(rdma);
  CHECK_DOCA(doca_rdma_set_permissions(rdma, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE));
  CHECK_DOCA(doca_rdma_set_grh_enabled(rdma, 1));  // RoCE needs GRH; GID index auto-selected
  { const char *gi = getenv("BUDDY_GID_INDEX"); if (gi && *gi) CHECK_DOCA(doca_rdma_set_gid_index(rdma, atoi(gi))); }

  struct doca_pe *pe;
  CHECK_DOCA(doca_pe_create(&pe));
  CHECK_DOCA(doca_pe_connect_ctx(pe, ctx));
  CHECK_DOCA(doca_rdma_task_send_set_conf(rdma, send_cb, send_err, NUM_TASKS));
  CHECK_DOCA(doca_rdma_task_receive_set_conf(rdma, recv_cb, recv_err, NUM_TASKS));
  union doca_data cu = { .ptr = &st };
  doca_ctx_set_user_data(ctx, cu);

  struct doca_mmap *mmap;
  CHECK_DOCA(doca_mmap_create(&mmap));
  CHECK_DOCA(doca_mmap_add_dev(mmap, dev));
  CHECK_DOCA(doca_mmap_set_permissions(mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE));
  CHECK_DOCA(doca_mmap_set_memrange(mmap, data, LEN));
  CHECK_DOCA(doca_mmap_start(mmap));
  struct doca_buf_inventory *inv;
  CHECK_DOCA(doca_buf_inventory_create(4, &inv));
  CHECK_DOCA(doca_buf_inventory_start(inv));

  CHECK_DOCA(doca_ctx_start(ctx));

  // export our connection blob, swap with peer over TCP, connect
  const void *blob; size_t blob_len; struct doca_rdma_connection *conn;
  CHECK_DOCA(doca_rdma_export(rdma, &blob, &blob_len, &conn));

  int sock;
  if (server) { int ls = tcp_listen(atoi(argv[2])); sock = accept(ls, NULL, NULL); }
  else        { sock = tcp_connect(argv[2], atoi(argv[3])); }
  uint64_t bl = blob_len;
  full_write(sock, (char *)&bl, sizeof(bl));
  full_write(sock, (char *)blob, blob_len);
  uint64_t pbl; full_read(sock, (char *)&pbl, sizeof(pbl));
  char *pblob = new char[pbl]; full_read(sock, pblob, pbl);
  CHECK_DOCA(doca_rdma_connect(rdma, pblob, pbl, conn));

  // drive the ctx to RUNNING (connection established)
  enum doca_ctx_states cs;
  do { doca_pe_progress(pe); doca_ctx_get_state(ctx, &cs); } while (cs != DOCA_CTX_STATE_RUNNING);

  if (server) {
    struct doca_buf *dst;
    CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, mmap, data, LEN, &dst));
    struct doca_rdma_task_receive *t;
    CHECK_DOCA(doca_rdma_task_receive_allocate_init(rdma, dst, cu, &t));
    st.remaining++;
    CHECK_DOCA(doca_task_submit(doca_rdma_task_receive_as_task(t)));
    char x = 1; full_write(sock, &x, 1);           // tell client the recv is posted
  } else {
    char x; full_read(sock, &x, 1);                // wait until server posted recv
    struct doca_buf *src;
    CHECK_DOCA(doca_buf_inventory_buf_get_by_addr(inv, mmap, data, LEN, &src));
    CHECK_DOCA(doca_buf_set_data(src, data, LEN));
    struct doca_rdma_task_send *t;
    CHECK_DOCA(doca_rdma_task_send_allocate_init(rdma, conn, src, cu, &t));
    st.remaining++;
    CHECK_DOCA(doca_task_submit(doca_rdma_task_send_as_task(t)));
  }

  while (st.remaining > 0) doca_pe_progress(pe);

  if (server) {
    bool ok = st.rlen == LEN;
    for (size_t i = 0; ok && i < LEN; i++) if ((unsigned char)data[i] != 0xC5) ok = false;
    std::cout << "server: recv " << st.rlen << "B verify " << (ok ? "OK" : "FAIL") << std::endl;
    return ok ? 0 : 1;
  }
  std::cout << "client: send done" << std::endl;
  return 0;
}
