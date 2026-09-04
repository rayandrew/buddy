#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_rdma.h>

#include "dpa_d2d.h"
#include "dpa_hostleg.h"
#include "sockets.h"
#include "util.h"

extern struct doca_dpa_app *buddy_dpa_app;
extern "C" doca_dpa_func_t d2d_handler;
extern "C" doca_dpa_func_t d2d_arm;
extern "C" doca_dpa_func_t d2d_generate;

namespace buddy::rdma {

/* Mirrors struct d2d_engine in dpa_d2d_dev.c; the definitions must agree field for field. */
struct d2d_engine_arg {
	uint64_t magic;
	uint64_t ctx;
	uint64_t comp;
	uint32_t mmap;
	uint64_t rdma[8];
	uint32_t num_peers;

	uint64_t rx_base;
	uint64_t buf_size;
	uint32_t nrx;

	uint8_t route[DpaD2D::kMaxRanks];
	uint32_t num_ranks;

	uint64_t pending_addr;
	uint32_t pend_head;
	uint32_t pend_tail;
	uint32_t refcount[512];

	uint64_t rxfifo_addr;
	uint32_t rx_head;
	uint32_t rx_tail;

	uint64_t tx_base;
	uint64_t tx_len;
	uint64_t ack_len;
	uint32_t ntx;
	uint32_t next_tx;

	uint64_t wakes;
	uint64_t tx_msgs;
	uint64_t rx_msgs;
	uint64_t records;
	uint64_t forwards;
	uint64_t local;
	uint64_t bad_records;
	uint64_t zerocopy;
	uint64_t tx_full;
	uint32_t held;
	uint32_t tx_inflight;
	uint32_t freed;
	uint64_t defer_addr;
	uint32_t defer_head;
	uint32_t defer_tail;
	uint32_t no_zerocopy;
	uint32_t gen_paused;
	uint64_t errors;
	uint64_t last_err;
	uint64_t first_err_rx;
	uint64_t first_err_tx;
};

struct DpaD2D::Engine {
	doca_rdma *rdma = nullptr;
	doca_ctx *ctx = nullptr;
	doca_dpa_completion *comp = nullptr;
	doca_dpa_thread *thread = nullptr;
	doca_rdma_connection *conn = nullptr;
	uint64_t rdma_handle = 0;
	uint64_t arg_dev = 0;
	uint64_t rx_base = 0;
	uint64_t tx_base = 0;
};

static const uint32_t kPerms = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
			       DOCA_ACCESS_FLAG_RDMA_WRITE;

static const char *env_str(const char *k, const char *d)
{
	const char *e = getenv(k);
	return e && *e ? e : d;
}

static doca_dev *open_dev(const char *ibdev)
{
	doca_devinfo **list;
	uint32_t n = 0;
	char name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	CHECK_DOCA(doca_devinfo_create_list(&list, &n));
	for (uint32_t i = 0; i < n; i++) {
		if (doca_devinfo_get_ibdev_name(list[i], name, sizeof(name)) != DOCA_SUCCESS) continue;
		if (strncmp(ibdev, name, sizeof(name)) != 0) continue;
		doca_dev *d = nullptr;
		if (doca_dev_open(list[i], &d) == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(list);
			return d;
		}
	}
	doca_devinfo_destroy_list(list);
	FAIL("no doca device ibdev=" << ibdev);
}

DpaD2D::DpaD2D(unsigned num_engines, unsigned bufs_per_engine, size_t buf_size)
	: num_engines(num_engines), bufs_per_engine(bufs_per_engine), buf_size(buf_size)
{
	if (num_engines == 0 || num_engines > kMaxEngines)
		FAIL("engines must be 1.." << kMaxEngines << ", got " << num_engines);
	if (bufs_per_engine > 512)
		FAIL("bufs_per_engine is capped at 512 by the device refcount table");
	memset(route, kLocal, sizeof(route));

	dev = open_dev(env_str("BUDDY_RDMA_DEV", "mlx5_2"));
	pf_dev = open_dev(env_str("BUDDY_DPA_PF_DEV", "mlx5_0"));

	/* A DPA process lives on the PF; create there and extend onto the RDMA scalable function. */
	CHECK_DOCA(doca_dpa_create(pf_dev, &pf_dpa));
	CHECK_DOCA(doca_dpa_set_app(pf_dpa, buddy_dpa_app));
	CHECK_DOCA(doca_dpa_start(pf_dpa));
	if (dev != pf_dev)
		CHECK_DOCA(doca_dpa_device_extend(pf_dpa, dev, &dpa));
	else
		dpa = pf_dpa;

	/* Receive pools for every engine, then a transmit pool for every engine. All of it is DPA
	 * memory: the kernel reads it with a plain load and the NIC delivers into it directly. */
	const size_t per_engine = (size_t)bufs_per_engine * buf_size;
	const size_t pool = per_engine * num_engines * 2;
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, pool, &pool_dev));
	CHECK_DOCA(doca_dpa_memset(dpa, pool_dev, 0, pool));

	CHECK_DOCA(doca_mmap_create(&mmap));
	CHECK_DOCA(doca_mmap_set_permissions(mmap, kPerms));
	CHECK_DOCA(doca_mmap_set_dpa_memrange(mmap, dpa, pool_dev, pool));
	CHECK_DOCA(doca_mmap_start(mmap));

	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(d2d_engine_arg) * num_engines, &args_dev));
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint32_t) * 8192 * num_engines, &pending_dev));
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint32_t) * 8192 * num_engines, &rxfifo_dev));
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint32_t) * 512 * num_engines, &defer_dev));

	engines = new Engine[num_engines];
	for (unsigned i = 0; i < num_engines; i++) {
		engines[i].rx_base = pool_dev + (size_t)i * per_engine;
		engines[i].tx_base = pool_dev + per_engine * num_engines + (size_t)i * per_engine;
		engines[i].arg_dev = args_dev + sizeof(d2d_engine_arg) * i;
		init_engine(engines[i], i);
	}
}

void DpaD2D::init_engine(Engine &e, unsigned)
{
	CHECK_DOCA(doca_dpa_thread_create(dpa, &e.thread));
	CHECK_DOCA(doca_dpa_thread_set_func_arg(e.thread, &d2d_handler, e.arg_dev));
	CHECK_DOCA(doca_dpa_thread_start(e.thread));

	CHECK_DOCA(doca_dpa_completion_create(dpa, 8192, &e.comp));
	CHECK_DOCA(doca_dpa_completion_set_thread(e.comp, e.thread));
	CHECK_DOCA(doca_dpa_completion_start(e.comp));

	CHECK_DOCA(doca_rdma_create(dev, &e.rdma));
	e.ctx = doca_rdma_as_ctx(e.rdma);
	CHECK_DOCA(doca_ctx_set_datapath_on_dpa(e.ctx, dpa));
	CHECK_DOCA(doca_rdma_set_permissions(e.rdma, kPerms));
	CHECK_DOCA(doca_rdma_set_grh_enabled(e.rdma, 1));
	CHECK_DOCA(doca_rdma_set_gid_index(e.rdma, (uint32_t)atoi(env_str("BUDDY_GID_INDEX", "0"))));
	CHECK_DOCA(doca_rdma_set_max_num_connections(e.rdma, 1));
	CHECK_DOCA(doca_rdma_set_rnr_retry_count(e.rdma, 7));
	CHECK_DOCA(doca_rdma_set_send_queue_size(e.rdma, 1024));
	{
		const doca_error_t rq = doca_rdma_set_recv_queue_size(e.rdma, 1024);
		if (rq != DOCA_SUCCESS && rq != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(rq));
		const doca_error_t bl = doca_rdma_task_receive_set_dst_buf_list_len(e.rdma, 1);
		if (bl != DOCA_SUCCESS && bl != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(bl));
	}
	CHECK_DOCA(doca_rdma_dpa_completion_attach(e.rdma, e.comp));
	CHECK_DOCA(doca_ctx_start(e.ctx));
	CHECK_DOCA(doca_rdma_get_dpa_handle(e.rdma, &e.rdma_handle));
}

void DpaD2D::connect(int sock, bool is_server)
{
	/* Engine order is the same on both ends, so engine i pairs with the peer's engine i. */
	for (unsigned i = 0; i < num_engines; i++) {
		Engine &e = engines[i];
		const void *blob;
		size_t blob_len;
		CHECK_DOCA(doca_rdma_export(e.rdma, &blob, &blob_len, &e.conn));
		uint64_t bl = blob_len;
		full_write(sock, (char *)&bl, sizeof(bl));
		full_write(sock, (char *)blob, blob_len);
		uint64_t pbl;
		full_read(sock, (char *)&pbl, sizeof(pbl));
		char *pblob = new char[pbl];
		full_read(sock, pblob, pbl);
		CHECK_DOCA(doca_rdma_connect(e.rdma, pblob, pbl, e.conn));
		delete[] pblob;
	}
	(void)is_server;
}

void DpaD2D::wait_connected(double timeout_s)
{
	const auto deadline = std::chrono::steady_clock::now() +
			      std::chrono::duration<double>(timeout_s);
	for (unsigned i = 0; i < num_engines; i++) {
		for (;;) {
			doca_ctx_states cs;
			doca_ctx_get_state(engines[i].ctx, &cs);
			if (cs == DOCA_CTX_STATE_RUNNING) break;
			if (std::chrono::steady_clock::now() > deadline)
				FAIL("dpa d2d engine " << i << " did not reach RUNNING");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

bool DpaD2D::host_leg(uint16_t port, double timeout_s)
{
	hostleg = new HostLeg(dpa, dev);
	hostleg->attach(engines[0].comp);
	hostleg->listen(port);
	return hostleg->accept_one(timeout_s);
}

void DpaD2D::route_to_peer(int rank)
{
	if (rank < 0 || (unsigned)rank >= kMaxRanks) FAIL("rank out of range: " << rank);
	route[rank] = 0;
}

void DpaD2D::set_ack(size_t len) { ack_len = len; }

void DpaD2D::set_zerocopy(bool on) { no_zerocopy = !on; }

void DpaD2D::start()
{
	doca_dpa_dev_t dpa_handle = 0;
	CHECK_DOCA(doca_dpa_get_dpa_handle(dpa, &dpa_handle));

	for (unsigned i = 0; i < num_engines; i++) {
		Engine &e = engines[i];
		uint64_t comp_handle = 0;
		CHECK_DOCA(doca_dpa_completion_get_dpa_handle(e.comp, &comp_handle));

		d2d_engine_arg a = {};
		a.magic = 0x0D2D0FEEDULL;
		a.ctx = dpa_handle;
		a.comp = comp_handle;
		CHECK_DOCA(doca_mmap_dev_get_dpa_handle(mmap, dev, (doca_dpa_dev_mmap_t *)&a.mmap));
		/* The host leg replaces engine 0's receive side: the kernel posts and forwards on
		 * rdma[0], so pointing it at the host queue pair is the whole change. */
		a.rdma[0] = (hostleg && i == 0) ? hostleg->dpa_handle() : e.rdma_handle;
		a.num_peers = 1;
		a.rx_base = e.rx_base;
		a.tx_base = e.tx_base;
		a.buf_size = buf_size;
		a.nrx = bufs_per_engine;
		a.ntx = bufs_per_engine;
		a.ack_len = ack_len;
		a.no_zerocopy = no_zerocopy ? 1 : 0;
		a.pending_addr = pending_dev + (uint64_t)sizeof(uint32_t) * 8192 * i;
		a.rxfifo_addr = rxfifo_dev + (uint64_t)sizeof(uint32_t) * 8192 * i;
		a.defer_addr = defer_dev + (uint64_t)sizeof(uint32_t) * 512 * i;
		a.num_ranks = kMaxRanks;
		memcpy(a.route, route, sizeof(route));
		CHECK_DOCA(doca_dpa_h2d_memcpy(dpa, e.arg_dev, &a, sizeof(a)));

		CHECK_DOCA(doca_dpa_thread_run(e.thread));
		uint64_t rc = 0;
		CHECK_DOCA(doca_dpa_rpc(dpa, &d2d_arm, &rc, e.arg_dev));
	}
}

void DpaD2D::generate(unsigned window, unsigned records)
{
	/* Fill the transmit pool with real records, so the device walk has the shape buddy produces:
	 * many small requests aggregated into one buffer. */
	if (records) {
		const size_t each = buf_size / records;
		if (each <= 8) FAIL("records=" << records << " leaves no payload in " << buf_size);
		char *tmpl = (char *)calloc(1, buf_size);
		size_t pos = 0;
		for (unsigned i = 0; i < records && pos + each <= buf_size; i++, pos += each) {
			const uint32_t size = (uint32_t)(each - 8);
			const int32_t dst = (int32_t)(i % kMaxRanks);
			memcpy(tmpl + pos, &size, 4);
			memcpy(tmpl + pos + 4, &dst, 4);
		}
		for (unsigned i = 0; i < num_engines; i++)
			for (unsigned b = 0; b < bufs_per_engine; b++)
				CHECK_DOCA(doca_dpa_h2d_memcpy(dpa,
							       engines[i].tx_base +
								       (uint64_t)b * buf_size,
							       tmpl, buf_size));
		free(tmpl);
	}
	for (unsigned i = 0; i < num_engines; i++) {
		uint64_t rc = 0;
		/* Deliberately short of the buffer. buddy sends d2d_size-16 into a d2d_size receive, and
		 * a message that exactly fills its receive is a marginal case worth not sitting on. */
		CHECK_DOCA(doca_dpa_rpc(dpa, &d2d_generate, &rc, engines[i].arg_dev,
					(uint64_t)window, (uint64_t)(buf_size - 64)));
	}
}

DpaD2D::stats DpaD2D::sample() const
{
	stats s = {};
	for (unsigned i = 0; i < num_engines; i++) {
		d2d_engine_arg a = {};
		doca_dpa_d2h_memcpy(dpa, &a, engines[i].arg_dev, sizeof(a));
		s.wakes += a.wakes;
		s.tx_msgs += a.tx_msgs;
		s.rx_msgs += a.rx_msgs;
		s.records += a.records;
		s.forwards += a.forwards;
		s.local += a.local;
		s.bad_records += a.bad_records;
		s.zerocopy += a.zerocopy;
		s.tx_full += a.tx_full;
		s.errors += a.errors;
		if (a.last_err) {
			s.last_err = a.last_err;
			s.first_err_rx = a.first_err_rx;
			s.first_err_tx = a.first_err_tx;
		}
	}
	return s;
}

DpaD2D::~DpaD2D()
{
	delete hostleg;
	for (unsigned i = 0; i < num_engines; i++) {
		Engine &e = engines[i];
		if (e.ctx) doca_ctx_stop(e.ctx);
		if (e.rdma) doca_rdma_destroy(e.rdma);
		if (e.comp) doca_dpa_completion_destroy(e.comp);
		if (e.thread) doca_dpa_thread_destroy(e.thread);
	}
	if (mmap) doca_mmap_destroy(mmap);
	/* Before the process is destroyed, or the device heap is left allocated and the next run
	 * starts short of memory. */
	if (dpa) {
		if (defer_dev) doca_dpa_mem_free(dpa, defer_dev);
		if (rxfifo_dev) doca_dpa_mem_free(dpa, rxfifo_dev);
		if (pending_dev) doca_dpa_mem_free(dpa, pending_dev);
		if (args_dev) doca_dpa_mem_free(dpa, args_dev);
		if (pool_dev) doca_dpa_mem_free(dpa, pool_dev);
	}
	if (dpa && dpa != pf_dpa) doca_dpa_destroy(dpa);
	if (pf_dpa) doca_dpa_destroy(pf_dpa);
	if (pf_dev && pf_dev != dev) doca_dev_close(pf_dev);
	if (dev) doca_dev_close(dev);
	delete[] engines;
}

} // namespace buddy::rdma
