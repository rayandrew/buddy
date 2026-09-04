/* Does an x86 host talk to a queue pair whose data path is on the DPU's DPA?
 *
 * This is the gating question for moving buddy's host leg onto the DPA. The host side is ordinary
 * DOCA RDMA on the CPU, the DPU side is DOCA RDMA with doca_ctx_set_datapath_on_dpa, and the two
 * exchange the same export blob DpaD2D uses. The host runs a newer DOCA than the DPU, so blob
 * compatibility across that skew is exactly what is being tested.
 *
 *   DPU:  dpa-d2d --server
 *   host: dpa-hostleg --peer <dpu-ip>
 */
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#define PORT 18523
#define SLOT 4096
#define NSLOT 16

static void die(const char *what, doca_error_t e)
{
	fprintf(stderr, "FAIL: %s: %s\n", what, doca_error_get_descr(e));
	exit(1);
}
#define CK(call)                                                                                   \
	do {                                                                                       \
		const doca_error_t _e = (call);                                                    \
		if (_e != DOCA_SUCCESS) die(#call, _e);                                            \
	} while (0)

static int peer_sock(const char *peer)
{
	struct sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(PORT);
	inet_pton(AF_INET, peer, &a.sin_addr);
	for (int i = 0; i < 100; i++) {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (!connect(s, (struct sockaddr *)&a, sizeof(a))) return s;
		close(s);
		usleep(100000);
	}
	return -1;
}

static void full_rw(int s, void *p, size_t n, bool write_it)
{
	char *c = (char *)p;
	while (n) {
		const ssize_t r = write_it ? write(s, c, n) : read(s, c, n);
		if (r <= 0) { perror("socket"); exit(1); }
		c += r;
		n -= (size_t)r;
	}
}

static doca_dev *open_dev(const char *ibdev)
{
	doca_devinfo **list;
	uint32_t n = 0;
	char name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	CK(doca_devinfo_create_list(&list, &n));
	for (uint32_t i = 0; i < n; i++) {
		if (doca_devinfo_get_ibdev_name(list[i], name, sizeof(name)) != DOCA_SUCCESS) continue;
		if (strncmp(ibdev, name, sizeof(name)) != 0) continue;
		doca_dev *d = nullptr;
		if (doca_dev_open(list[i], &d) == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(list);
			return d;
		}
	}
	fprintf(stderr, "FAIL: no doca device %s\n", ibdev);
	exit(1);
}

int main(int argc, char **argv)
{
	const char *peer = nullptr;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--peer") && i + 1 < argc) peer = argv[++i];
	if (!peer) { printf("usage: --peer <dpu-ip>\n"); return 1; }

	const char *ibdev = getenv("BUDDY_RDMA_DEV") ? getenv("BUDDY_RDMA_DEV") : "mlx5_2";
	const uint32_t perms = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
			       DOCA_ACCESS_FLAG_RDMA_WRITE;

	doca_dev *dev = open_dev(ibdev);
	char *mem = (char *)aligned_alloc(64, (size_t)SLOT * NSLOT);
	memset(mem, 0, (size_t)SLOT * NSLOT);

	doca_mmap *mmap = nullptr;
	CK(doca_mmap_create(&mmap));
	CK(doca_mmap_add_dev(mmap, dev));
	CK(doca_mmap_set_permissions(mmap, perms));
	CK(doca_mmap_set_memrange(mmap, mem, (size_t)SLOT * NSLOT));
	CK(doca_mmap_start(mmap));

	doca_pe *pe = nullptr;
	CK(doca_pe_create(&pe));

	doca_rdma *rdma = nullptr;
	CK(doca_rdma_create(dev, &rdma));
	doca_ctx *ctx = doca_rdma_as_ctx(rdma);
	CK(doca_rdma_set_permissions(rdma, perms));
	CK(doca_rdma_set_grh_enabled(rdma, 1));
	CK(doca_rdma_set_gid_index(rdma, (uint32_t)(getenv("BUDDY_GID_INDEX")
							     ? atoi(getenv("BUDDY_GID_INDEX"))
							     : 0)));
	CK(doca_rdma_set_max_num_connections(rdma, 1));
	CK(doca_pe_connect_ctx(pe, ctx));
	CK(doca_ctx_start(ctx));

	int sock = peer_sock(peer);
	if (sock < 0) { fprintf(stderr, "FAIL: socket\n"); return 1; }

	doca_rdma_connection *conn = nullptr;
	const void *blob;
	size_t blob_len;
	CK(doca_rdma_export(rdma, &blob, &blob_len, &conn));
	uint64_t bl = blob_len;
	full_rw(sock, &bl, sizeof(bl), true);
	full_rw(sock, (void *)blob, blob_len, true);
	uint64_t pbl = 0;
	full_rw(sock, &pbl, sizeof(pbl), false);
	printf("host blob %zu bytes, peer blob %lu bytes\n", blob_len, pbl);
	char *pblob = new char[pbl];
	full_rw(sock, pblob, pbl, false);

	const doca_error_t ce = doca_rdma_connect(rdma, pblob, pbl, conn);
	if (ce != DOCA_SUCCESS) {
		printf("RESULT: connect across the version skew FAILED: %s\n",
		       doca_error_get_descr(ce));
		return 2;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	for (;;) {
		doca_ctx_states st;
		doca_pe_progress(pe);
		doca_ctx_get_state(ctx, &st);
		if (st == DOCA_CTX_STATE_RUNNING) break;
		if (std::chrono::steady_clock::now() > deadline) {
			printf("RESULT: context never reached RUNNING\n");
			return 3;
		}
	}
	printf("RESULT: an x86 DOCA RDMA context connected to a DPA-datapath queue pair\n");
	close(sock);
	return 0;
}
