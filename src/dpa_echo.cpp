/* Host half of the DPA-native echo: sets the device up, starts traffic, then samples a counter.
 *
 * The point is what it does NOT do. After echo_start the Arm issues no submits, polls no ring and
 * performs no cache maintenance; every message is received, echoed and re-armed on the DPA. The
 * rate it reports is therefore what the DPA leg can do with the Arm out of the data path.
 *
 *   node A: dpa-echo --server        node B: dpa-echo --peer <A-ip>
 */
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_rdma.h>

#include "sockets.h"
#include "util.h"

extern struct doca_dpa_app *buddy_dpa_app;
extern "C" doca_dpa_func_t echo_handler;
extern "C" doca_dpa_func_t echo_arm;
extern "C" doca_dpa_func_t echo_start;

/* Mirrors struct echo_arg in dpa_echo_dev.c. */
struct echo_arg {
	uint64_t ctx;
	uint64_t comp;
	uint64_t rdma;
	uint32_t mmap;

	uint64_t recv_base;
	uint64_t send_base;
	uint64_t slot_size;
	uint32_t nslot;

	uint64_t handled;
	uint64_t errors;
	uint64_t last_err;
	uint32_t next_recv;
	uint32_t next_send;

	uint64_t target;
	uint64_t msg_len;
	uint64_t burst;
	uint64_t in_flight;
	uint32_t parse;
	uint64_t records;
	uint64_t bad_records;
};

struct request_head {
	uint32_t size;
	int32_t dst;
};

#define PORT 18521

static int peer_sock(const char *peer)
{
	struct sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(PORT);
	if (!peer) {
		int l = socket(AF_INET, SOCK_STREAM, 0), on = 1;
		setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		a.sin_addr.s_addr = INADDR_ANY;
		if (bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, 1)) return -1;
		int s = accept(l, NULL, NULL);
		close(l);
		return s;
	}
	inet_pton(AF_INET, peer, &a.sin_addr);
	for (int i = 0; i < 100; i++) {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (!connect(s, (struct sockaddr *)&a, sizeof(a))) return s;
		close(s);
		usleep(100000);
	}
	return -1;
}

static struct doca_dev *open_dev(const char *ibdev)
{
	struct doca_devinfo **list;
	uint32_t n = 0;
	char name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	CHECK_DOCA(doca_devinfo_create_list(&list, &n));
	for (uint32_t i = 0; i < n; i++) {
		if (doca_devinfo_get_ibdev_name(list[i], name, sizeof(name)) != DOCA_SUCCESS) continue;
		if (strncmp(ibdev, name, sizeof(name)) != 0) continue;
		struct doca_dev *d = NULL;
		if (doca_dev_open(list[i], &d) == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(list);
			return d;
		}
	}
	doca_devinfo_destroy_list(list);
	FAIL("no doca device ibdev=" << ibdev);
}

static const char *env_str(const char *k, const char *dflt)
{
	const char *e = getenv(k);
	return e && *e ? e : dflt;
}

static int env_int(const char *k, int dflt)
{
	const char *e = getenv(k);
	return e && *e ? atoi(e) : dflt;
}

int main(int argc, char **argv)
{
	const char *peer = NULL;
	bool server = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--server")) server = true;
		else if (!strcmp(argv[i], "--peer") && i + 1 < argc) peer = argv[++i];
	}
	if (!server && !peer) { printf("usage: --server | --peer <ip>\n"); return 1; }

	const unsigned nslot = (unsigned)env_int("ECHO_SLOTS", 64);
	const size_t slot = (size_t)env_int("ECHO_SIZE", 4096);
	const unsigned window = (unsigned)env_int("ECHO_WINDOW", 32);
	const int seconds = env_int("ECHO_SECONDS", 10);

	const uint32_t perms = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
			       DOCA_ACCESS_FLAG_RDMA_WRITE;

	doca_dev *dev = open_dev(env_str("BUDDY_RDMA_DEV", "mlx5_2"));
	doca_dev *pf_dev = open_dev(env_str("BUDDY_DPA_PF_DEV", "mlx5_0"));

	/* A DPA process lives on the PF; create there and extend onto the RDMA scalable function. */
	doca_dpa *pf_dpa = nullptr, *dpa = nullptr;
	CHECK_DOCA(doca_dpa_create(pf_dev, &pf_dpa));
	CHECK_DOCA(doca_dpa_set_app(pf_dpa, buddy_dpa_app));
	/* We never print from the device, and the log stream is a separate device channel that
	 * has to be set up before start. Skipping it removes that dependency. */
	CHECK_DOCA(doca_dpa_set_log_level(pf_dpa, DOCA_DPA_DEV_LOG_LEVEL_DISABLE));
	CHECK_DOCA(doca_dpa_start(pf_dpa));
	if (dev != pf_dev)
		CHECK_DOCA(doca_dpa_device_extend(pf_dpa, dev, &dpa));
	else
		dpa = pf_dpa;

	/* The buffers live on the DPA, not on the Arm. This is the whole point: the kernel reaches
	 * them with an ordinary load and the NIC delivers into them directly. */
	const size_t pool = (size_t)nslot * slot * 2;
	doca_dpa_dev_uintptr_t buf_dev = 0;
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, pool, &buf_dev));
	CHECK_DOCA(doca_dpa_memset(dpa, buf_dev, 0xAB, pool));

	/* Fill the send buffers with real request records so the device walk has something to parse.
	 * ECHO_RECORDS records share the slot evenly, which is how buddy aggregates. */
	const int records = env_int("ECHO_RECORDS", 0);
	if (records > 0) {
		const size_t each = slot / (size_t)records;
		if (each <= sizeof(request_head))
			FAIL("ECHO_RECORDS=" << records << " leaves no room for a payload in "
			     << slot << " bytes");
		char *tmpl = (char *)calloc(1, slot);
		size_t pos = 0;
		for (int i = 0; i < records && pos + each <= slot; i++, pos += each) {
			request_head h = {(uint32_t)(each - sizeof(request_head)), i % 4};
			memcpy(tmpl + pos, &h, sizeof(h));
		}
		for (unsigned i = 0; i < nslot; i++)
			CHECK_DOCA(doca_dpa_h2d_memcpy(dpa,
						       buf_dev + (uint64_t)(nslot + i) * slot,
						       tmpl, slot));
		free(tmpl);
	}

	doca_mmap *mmap = nullptr;
	CHECK_DOCA(doca_mmap_create(&mmap));
	CHECK_DOCA(doca_mmap_set_permissions(mmap, perms));
	CHECK_DOCA(doca_mmap_set_dpa_memrange(mmap, dpa, buf_dev, pool));
	CHECK_DOCA(doca_mmap_start(mmap));

	doca_dpa_dev_uintptr_t arg_dev = 0;
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(echo_arg), &arg_dev));

	/* Event driven: the thread is triggered by the completion context and reschedules itself, so
	 * it holds an execution unit only while there is work. */
	doca_dpa_thread *thread = nullptr;
	CHECK_DOCA(doca_dpa_thread_create(dpa, &thread));
	CHECK_DOCA(doca_dpa_thread_set_func_arg(thread, &echo_handler, arg_dev));
	CHECK_DOCA(doca_dpa_thread_start(thread));

	doca_dpa_completion *comp = nullptr;
	CHECK_DOCA(doca_dpa_completion_create(dpa, (unsigned)env_int("ECHO_CQ", 8192), &comp));
	CHECK_DOCA(doca_dpa_completion_set_thread(comp, thread));
	CHECK_DOCA(doca_dpa_completion_start(comp));

	doca_rdma *rdma = nullptr;
	CHECK_DOCA(doca_rdma_create(dev, &rdma));
	doca_ctx *ctx = doca_rdma_as_ctx(rdma);
	CHECK_DOCA(doca_ctx_set_datapath_on_dpa(ctx, dpa));
	CHECK_DOCA(doca_rdma_set_permissions(rdma, perms));
	CHECK_DOCA(doca_rdma_set_grh_enabled(rdma, 1));
	CHECK_DOCA(doca_rdma_set_gid_index(rdma, (uint32_t)env_int("BUDDY_GID_INDEX", 0)));
	CHECK_DOCA(doca_rdma_set_max_num_connections(rdma, 1));
	CHECK_DOCA(doca_rdma_set_rnr_retry_count(rdma, 7));
	CHECK_DOCA(doca_rdma_set_send_queue_size(rdma, (uint32_t)env_int("ECHO_SQ", 1024)));
	{
		const doca_error_t rq = doca_rdma_set_recv_queue_size(rdma, (uint32_t)env_int("ECHO_RQ", 1024));
		if (rq != DOCA_SUCCESS && rq != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(rq));
		const doca_error_t bl = doca_rdma_task_receive_set_dst_buf_list_len(rdma, 1);
		if (bl != DOCA_SUCCESS && bl != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(bl));
	}
	CHECK_DOCA(doca_rdma_dpa_completion_attach(rdma, comp));
	CHECK_DOCA(doca_ctx_start(ctx));

	uint64_t rdma_handle = 0;
	CHECK_DOCA(doca_rdma_get_dpa_handle(rdma, &rdma_handle));

	int sock = peer_sock(server ? NULL : peer);
	if (sock < 0) FAIL("socket failed");
	{
		doca_rdma_connection *conn = nullptr;
		const void *blob;
		size_t blob_len;
		CHECK_DOCA(doca_rdma_export(rdma, &blob, &blob_len, &conn));
		uint64_t bl = blob_len;
		buddy::full_write(sock, (char *)&bl, sizeof(bl));
		buddy::full_write(sock, (char *)blob, blob_len);
		uint64_t pbl;
		buddy::full_read(sock, (char *)&pbl, sizeof(pbl));
		char *pblob = new char[pbl];
		buddy::full_read(sock, pblob, pbl);
		CHECK_DOCA(doca_rdma_connect(rdma, pblob, pbl, conn));
		delete[] pblob;
	}
	for (;;) {
		doca_ctx_states cs;
		doca_ctx_get_state(ctx, &cs);
		if (cs == DOCA_CTX_STATE_RUNNING) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	doca_dpa_dev_t dpa_handle = 0;
	CHECK_DOCA(doca_dpa_get_dpa_handle(dpa, &dpa_handle));
	uint64_t comp_handle = 0;
	CHECK_DOCA(doca_dpa_completion_get_dpa_handle(comp, &comp_handle));

	echo_arg a = {};
	a.ctx = dpa_handle;
	a.comp = comp_handle;
	a.rdma = rdma_handle;
	CHECK_DOCA(doca_mmap_dev_get_dpa_handle(mmap, dev, (doca_dpa_dev_mmap_t *)&a.mmap));
	a.recv_base = buf_dev;
	a.send_base = buf_dev + (uint64_t)nslot * slot;
	a.slot_size = slot;
	a.nslot = nslot;
	a.parse = (uint32_t)(records > 0);
	a.burst = (uint64_t)env_int("ECHO_BURST", 16);
	CHECK_DOCA(doca_dpa_h2d_memcpy(dpa, arg_dev, &a, sizeof(a)));

	CHECK_DOCA(doca_dpa_thread_run(thread));

	uint64_t rc = 0;
	CHECK_DOCA(doca_dpa_rpc(dpa, &echo_arm, &rc, arg_dev));

	/* Both sides must be armed before either sends, or the first message meets an empty queue. */
	{
		char sync = 1;
		buddy::full_write(sock, &sync, 1);
		buddy::full_read(sock, &sync, 1);
	}
	/* Post the window in chunks. One RPC posting the whole window dies above about 64 operations,
	 * so this separates the size of a single burst from the steady depth they add up to. */
	if (!server) {
		const unsigned chunk = (unsigned)env_int("ECHO_CHUNK", 16);
		for (unsigned done = 0; done < window; done += chunk) {
			const uint64_t n = std::min<unsigned>(chunk, window - done);
			CHECK_DOCA(doca_dpa_rpc(dpa, &echo_start, &rc, arg_dev, n, (uint64_t)slot));
		}
	}

	/* From here the Arm only samples a counter. It issues no work and polls no ring. Sampling once
	 * a second separates a steady rate from a burst that stalls, which one interval cannot. */
	echo_arg s = {}, prev = {};
	CHECK_DOCA(doca_dpa_d2h_memcpy(dpa, &prev, arg_dev, sizeof(prev)));
	for (int i = 0; i < seconds; i++) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		CHECK_DOCA(doca_dpa_d2h_memcpy(dpa, &s, arg_dev, sizeof(s)));
		printf("dpa-echo %s t=%2ds  %8lu msg/s  %10lu rec/s  handled=%-10lu errors=%lu bad_rec=%lu\n",
		       server ? "server" : "client", i + 1, s.handled - prev.handled,
		       s.records - prev.records, s.handled, s.errors, s.bad_records);
		fflush(stdout);
		prev = s;
	}

	{
		char sync = 1;
		buddy::full_write(sock, &sync, 1);
		buddy::full_read(sock, &sync, 1);
	}
	close(sock);
	return 0;
}
