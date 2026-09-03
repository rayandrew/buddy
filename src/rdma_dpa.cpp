#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <thread>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_rdma.h>

#include "rdma_dpa.h"
#include "sockets.h"
#include "util.h"

/* Produced by dpacc from rdma_dpa_dev.c; see CMakeLists. */
extern struct doca_dpa_app *buddy_dpa_app;
extern "C" doca_dpa_func_t fabric_kernel;

namespace buddy::rdma {

/* Mirrors struct fabric_arg in rdma_dpa_dev.c; the definitions must agree field for field. */
static constexpr uint64_t kArgMagic = 0x0DFAB0FEEDULL;

struct fabric_arg {
	uint64_t magic;
	uint64_t ctx;
	uint64_t comp;
	uint64_t rdma;

	uint32_t sub_mmap;
	uint64_t sub_addr;
	uint64_t sub_len;
	uint64_t sub_head;
	uint64_t consumed_addr;

	uint32_t ring_mmap;
	uint64_t ring_addr;
	uint64_t ring_len;
	uint64_t tail;
	uint64_t ring_consumed_addr;

	uint64_t local_mmap;
	uint64_t local_base;
	uint64_t remote_mmap[DpaFabric::kMaxConns];
	uint64_t remote_base[DpaFabric::kMaxConns];
	uint32_t num_conns;

	uint64_t wrid_addr;
	uint64_t wrid_head[DpaFabric::kMaxConns];
	uint64_t wrid_tail[DpaFabric::kMaxConns];

	uint64_t rwrid_addr;
	uint64_t rwrid_head;
	uint64_t rwrid_tail;

	uint64_t errors;
	uint64_t last_err;
	uint64_t stop_addr;
};

/* The DPA reaches Arm memory through a window it must explicitly write back, so those writes are not
 * plainly coherent with the Arm's caches. A spin on an ordinary load can therefore sit on a stale
 * line forever, which is why progress looked random. Invalidate the line before every such read. */
static inline void inv_line(const void *p)
{
	asm volatile("dc civac, %0" ::"r"(p) : "memory");
	asm volatile("dsb sy" ::: "memory");
}

/* The mirror: the DPA reads Arm memory, not the Arm's caches, so a slot written here is invisible
 * until its line is cleaned out. A slot is one 64B line, so payload and seq leave together and the
 * DPA never sees a half-written descriptor. */
static inline void flush_line(const void *p)
{
	asm volatile("dc cvac, %0" ::"r"(p) : "memory");
	asm volatile("dsb sy" ::: "memory");
}

/* dc civac is clean-and-invalidate, the only cache op available at EL0, so it writes a dirty line
 * back before invalidating it. memset leaves every ring line dirty, and the first poll of a slot
 * would then flush those stale zeros over a completion the DPA had already written. Clean the
 * ranges once up front so no dirty line ever meets a device write. */
static void flush_range(void *p, size_t len)
{
	for (char *q = (char *)p; q < (char *)p + len; q += 64)
		asm volatile("dc civac, %0" ::"r"(q) : "memory");
	asm volatile("dsb sy" ::: "memory");
}

static const char *dpa_ibdev()
{
	const char *e = getenv("BUDDY_RDMA_DEV");
	return e && *e ? e : "mlx5_2";
}

static uint32_t dpa_gid()
{
	const char *e = getenv("BUDDY_GID_INDEX");
	return e && *e ? atoi(e) : 0;
}

/* The physical function backing the RDMA SF; DPA processes can only be created there. */
static const char *dpa_pf_ibdev()
{
	const char *e = getenv("BUDDY_DPA_PF_DEV");
	return e && *e ? e : "mlx5_0";
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

static unsigned dpa_ring_slots()
{
	const char *e = getenv("BUDDY_DPA_RING");
	const unsigned n = e && *e ? (unsigned)strtoul(e, NULL, 10) : 16384;
	if (n & (n - 1)) FAIL("BUDDY_DPA_RING must be a power of two, got " << n);
	return n;
}

static const uint32_t kPerms = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
			       DOCA_ACCESS_FLAG_RDMA_WRITE;

DpaFabric::DpaFabric(unsigned num_connections, char *mem, size_t mem_len, unsigned num_lanes)
	: num_connections(num_connections), num_lanes(num_lanes ? num_lanes : 1), mem(mem),
	  mem_len(mem_len), ring_len(dpa_ring_slots())
{
	if (num_connections > kMaxConns) FAIL("dpa fabric supports at most " << kMaxConns << " peers");
	dev = open_dev(dpa_ibdev());
	pf_dev = open_dev(dpa_pf_ibdev());

	CHECK_DOCA(doca_dpa_create(pf_dev, &pf_dpa));
	CHECK_DOCA(doca_dpa_set_app(pf_dpa, buddy_dpa_app));
	CHECK_DOCA(doca_dpa_start(pf_dpa));
	if (dev != pf_dev)
		CHECK_DOCA(doca_dpa_device_extend(pf_dpa, dev, &dpa));
	else
		dpa = pf_dpa;

	CHECK_DOCA(doca_mmap_create(&mmap));
	CHECK_DOCA(doca_mmap_add_dev(mmap, dev));
	CHECK_DOCA(doca_mmap_set_permissions(mmap, kPerms));
	CHECK_DOCA(doca_mmap_set_memrange(mmap, mem, mem_len));
	CHECK_DOCA(doca_mmap_start(mmap));

	remote_mmap = new doca_mmap *[num_connections]();
	lanes = new Lane[this->num_lanes];
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(fabric_arg) * this->num_lanes, &args_dev));
	for (unsigned i = 0; i < this->num_lanes; i++) init_lane(lanes[i]);
}

void DpaFabric::init_lane(Lane &l)
{
	/* Both rings are registered on the PF: doca_dpa_dev_mmap_get_external_ptr only works for an
	 * mmap and handle from the PF context, not the extended one. */
	/* 64B-aligned: the DPA reaches these through a window with a 64B alignment restriction, and a
	 * misaligned range makes the kernel's writes invisible to the Arm with no error anywhere. */
	/* ring_consumed shares the mapping but its own cache line: without it the kernel would wrap and
	 * overwrite completions the proxy has not read yet, losing them silently. */
	const size_t ring_bytes = kLineSize + sizeof(ring_slot) * ring_len;
	l.ring_mem = (char *)aligned_alloc(kLineSize, ring_bytes);
	if (!l.ring_mem) FAIL("dpa ring allocation failed");
	memset(l.ring_mem, 0, ring_bytes);
	flush_range(l.ring_mem, ring_bytes);
	l.ring_consumed = (uint64_t *)l.ring_mem;
	l.ring = (ring_slot *)(l.ring_mem + kLineSize);
	CHECK_DOCA(doca_mmap_create(&l.ring_mmap));
	CHECK_DOCA(doca_mmap_add_dev(l.ring_mmap, pf_dev));
	CHECK_DOCA(doca_mmap_set_permissions(l.ring_mmap, kPerms));
	CHECK_DOCA(doca_mmap_set_memrange(l.ring_mmap, l.ring_mem, ring_bytes));
	CHECK_DOCA(doca_mmap_start(l.ring_mmap));

	/* sub_consumed shares the mapping but its own cache line. */
	const size_t sub_bytes = kLineSize + sizeof(submit_slot) * ring_len;
	l.sub_mem = (char *)aligned_alloc(kLineSize, sub_bytes);
	if (!l.sub_mem) FAIL("dpa submit ring allocation failed");
	memset(l.sub_mem, 0, sub_bytes);
	flush_range(l.sub_mem, sub_bytes);
	l.sub_consumed = (uint64_t *)l.sub_mem;
	l.sub = (submit_slot *)(l.sub_mem + kLineSize);
	CHECK_DOCA(doca_mmap_create(&l.sub_mmap));
	CHECK_DOCA(doca_mmap_add_dev(l.sub_mmap, pf_dev));
	CHECK_DOCA(doca_mmap_set_permissions(l.sub_mmap, kPerms));
	CHECK_DOCA(doca_mmap_set_memrange(l.sub_mmap, l.sub_mem, sub_bytes));
	CHECK_DOCA(doca_mmap_start(l.sub_mmap));

	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint64_t), &l.stop_dev));
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint64_t) * kMaxConns * kWridFifo, &l.wrid_dev));
	CHECK_DOCA(doca_dpa_memset(dpa, l.wrid_dev, 0, sizeof(uint64_t) * kMaxConns * kWridFifo));
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint64_t) * kWridFifo, &l.rwrid_dev));
	CHECK_DOCA(doca_dpa_memset(dpa, l.rwrid_dev, 0, sizeof(uint64_t) * kWridFifo));
	{
		const uint64_t zero = 0;
		CHECK_DOCA(doca_dpa_h2d_memcpy(dpa, l.stop_dev, (void *)&zero, sizeof(zero)));
	}
	/* A completion context still needs a thread object to attach to, but that thread is never
	 * run: the data path is the launched kernel, not a thread activation. */
	CHECK_DOCA(doca_dpa_thread_create(dpa, &l.thread));
	CHECK_DOCA(doca_dpa_thread_set_func_arg(l.thread, &fabric_kernel, 0));
	CHECK_DOCA(doca_dpa_thread_start(l.thread));

	CHECK_DOCA(doca_dpa_completion_create(dpa, 4096, &l.comp));
	CHECK_DOCA(doca_dpa_completion_set_thread(l.comp, l.thread));
	CHECK_DOCA(doca_dpa_completion_start(l.comp));

	CHECK_DOCA(doca_rdma_create(dev, &l.rdma));
	l.ctx = doca_rdma_as_ctx(l.rdma);
	CHECK_DOCA(doca_ctx_set_datapath_on_dpa(l.ctx, dpa));
	CHECK_DOCA(doca_rdma_set_permissions(l.rdma, kPerms));
	CHECK_DOCA(doca_rdma_set_grh_enabled(l.rdma, 1));
	/* Same GID as the other legs. Left unset the connection still establishes and the data lands
	 * wrong, which surfaces only as a RECV_ERR completion. */
	CHECK_DOCA(doca_rdma_set_gid_index(l.rdma, dpa_gid()));
	CHECK_DOCA(doca_rdma_set_max_num_connections(l.rdma, num_connections));
	CHECK_DOCA(doca_rdma_set_rnr_retry_count(l.rdma, 7));
	/* buddy sends d2d_size-16 (4080 B by default). Measured: the DPA path completes 1 KB sends and
	 * silently drops 4080 B ones. */
	{
		const doca_error_t mtu = doca_rdma_set_mtu(l.rdma, DOCA_MTU_SIZE_4K_BYTES);
		if (mtu != DOCA_SUCCESS && mtu != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(mtu));
	}

	/* Same depth as the other two legs so a comparison is not a comparison of defaults. Unlike the
	 * CPU data path, recv-queue sizing is honoured on the DPA data path, and leaving it at the
	 * default surfaces only as a RECV_ERR completion with no message. */
	{
		const struct doca_devinfo *info = doca_dev_as_devinfo(dev);
		uint32_t max_sq = 0, max_rq = 0;
		CHECK_DOCA(doca_rdma_cap_get_max_send_queue_size(info, &max_sq));
		CHECK_DOCA(doca_rdma_cap_get_max_recv_queue_size(info, &max_rq));
		CHECK_DOCA(doca_rdma_set_send_queue_size(l.rdma, std::min<uint32_t>(buddy::queue_depth(), max_sq)));
		const doca_error_t rq = doca_rdma_set_recv_queue_size(l.rdma, std::min<uint32_t>(buddy::queue_depth(), max_rq));
		if (rq != DOCA_SUCCESS && rq != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(rq));
		/* Measured: at the default list length each post_receive reserves four receive-queue
		 * entries, so only one receive in four is usable. buddy chains no buffers. */
		const doca_error_t bl = doca_rdma_task_receive_set_dst_buf_list_len(l.rdma, 1);
		if (bl != DOCA_SUCCESS && bl != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(bl));
	}
	CHECK_DOCA(doca_rdma_dpa_completion_attach(l.rdma, l.comp));
	CHECK_DOCA(doca_ctx_start(l.ctx));
	CHECK_DOCA(doca_rdma_get_dpa_handle(l.rdma, &l.rdma_handle));

	l.conns = new doca_rdma_connection *[num_connections]();
}

/* The kernels cannot run until every peer's mmap handle is known, so the argument blocks are filled
 * and the launch issued only after all connections are up.
 *
 * One launch for every lane: fabric_kernel never returns, so a second launch on this context would
 * queue behind the first and never start. Each DPA thread picks its lane by rank. */
void DpaFabric::launch_kernels()
{
	doca_dpa_dev_t dpa_handle = 0;
	CHECK_DOCA(doca_dpa_get_dpa_handle(dpa, &dpa_handle));

	fabric_arg *args = new fabric_arg[num_lanes]();
	for (unsigned n = 0; n < num_lanes; n++) {
		Lane &l = lanes[n];
		fabric_arg &a = args[n];
		uint64_t comp_handle = 0;
		CHECK_DOCA(doca_dpa_completion_get_dpa_handle(l.comp, &comp_handle));

		a.magic = kArgMagic;
		a.ctx = dpa_handle;
		a.comp = comp_handle;
		a.rdma = l.rdma_handle;
		CHECK_DOCA(doca_mmap_dev_get_dpa_handle(l.sub_mmap, pf_dev,
							(doca_dpa_dev_mmap_t *)&a.sub_mmap));
		a.sub_addr = (uint64_t)l.sub;
		a.sub_len = ring_len;
		a.consumed_addr = (uint64_t)l.sub_consumed;
		CHECK_DOCA(doca_mmap_dev_get_dpa_handle(l.ring_mmap, pf_dev,
							(doca_dpa_dev_mmap_t *)&a.ring_mmap));
		a.ring_addr = (uint64_t)l.ring;
		a.ring_len = ring_len;
		a.ring_consumed_addr = (uint64_t)l.ring_consumed;
		a.stop_addr = l.stop_dev;
		a.wrid_addr = l.wrid_dev;
		a.rwrid_addr = l.rwrid_dev;
		CHECK_DOCA(doca_mmap_dev_get_dpa_handle(mmap, dev, (doca_dpa_dev_mmap_t *)&a.local_mmap));
		a.local_base = (uint64_t)mem;
		a.num_conns = num_connections;
		for (unsigned i = 0; i < num_connections; i++) {
			CHECK_DOCA(doca_mmap_dev_get_dpa_handle(remote_mmap[i], dev,
								(doca_dpa_dev_mmap_t *)&a.remote_mmap[i]));
			void *rbase; size_t rlen;
			CHECK_DOCA(doca_mmap_get_memrange(remote_mmap[i], &rbase, &rlen));
			a.remote_base[i] = (uint64_t)rbase;
		}
	}

	CHECK_DOCA(doca_dpa_h2d_memcpy(dpa, args_dev, args, sizeof(fabric_arg) * num_lanes));
	delete[] args;

	/* Asynchronous: the call returns once the launch is submitted, and the kernels then own one
	 * execution unit each until their stop word is set. */
	CHECK_DOCA(doca_dpa_kernel_launch_update_set(dpa, NULL, 0, NULL, 0, num_lanes, &fabric_kernel,
						     args_dev));
}

DpaFabric::~DpaFabric()
{
	/* Release every spinning kernel's EU before tearing down anything they touch. */
	for (unsigned i = 0; i < num_lanes; i++) {
		if (!lanes[i].stop_dev) continue;
		const uint64_t one = 1;
		doca_dpa_h2d_memcpy(dpa, lanes[i].stop_dev, (void *)&one, sizeof(one));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	for (unsigned i = 0; i < num_lanes; i++) {
		Lane &l = lanes[i];
		if (l.ctx) doca_ctx_stop(l.ctx);
		if (l.rdma) doca_rdma_destroy(l.rdma);
		if (l.comp) doca_dpa_completion_destroy(l.comp);
		if (l.thread) doca_dpa_thread_destroy(l.thread);
		if (l.ring_mmap) doca_mmap_destroy(l.ring_mmap);
		if (l.sub_mmap) doca_mmap_destroy(l.sub_mmap);
		delete[] l.conns;
		free(l.ring_mem);
		free(l.sub_mem);
	}
	doca_mmap_destroy(mmap);
	for (unsigned i = 0; i < num_connections; i++)
		if (remote_mmap[i]) doca_mmap_destroy(remote_mmap[i]);
	if (dpa != pf_dpa) doca_dpa_destroy(dpa);
	doca_dpa_destroy(pf_dpa);
	if (pf_dev != dev) doca_dev_close(pf_dev);
	doca_dev_close(dev);
	delete[] remote_mmap;
	delete[] lanes;
}

void DpaFabric::connect(unsigned idx, int sock, bool is_server)
{
	/* Lane order is the same on both ends, so lane i pairs with the peer's lane i. */
	for (unsigned i = 0; i < num_lanes; i++) {
		Lane &l = lanes[i];
		const void *blob;
		size_t blob_len;
		CHECK_DOCA(doca_rdma_export(l.rdma, &blob, &blob_len, &l.conns[idx]));

		uint64_t bl = blob_len;
		full_write(sock, (char *)&bl, sizeof(bl));
		full_write(sock, (char *)blob, blob_len);
		uint64_t pbl;
		full_read(sock, (char *)&pbl, sizeof(pbl));
		char *pblob = new char[pbl];
		full_read(sock, pblob, pbl);
		CHECK_DOCA(doca_rdma_connect(l.rdma, pblob, pbl, l.conns[idx]));
		delete[] pblob;
	}

	const void *mdesc;
	size_t mdesc_len;
	CHECK_DOCA(doca_mmap_export_rdma(mmap, dev, &mdesc, &mdesc_len));
	uint64_t ml = mdesc_len;
	full_write(sock, (char *)&ml, sizeof(ml));
	full_write(sock, (char *)mdesc, mdesc_len);
	uint64_t pml;
	full_read(sock, (char *)&pml, sizeof(pml));
	char *pmdesc = new char[pml];
	full_read(sock, pmdesc, pml);
	CHECK_DOCA(doca_mmap_create_from_export(NULL, pmdesc, pml, dev, &remote_mmap[idx]));
	delete[] pmdesc;
	(void)is_server;
}

void DpaFabric::wait_connected(double timeout_s)
{
	const auto deadline = std::chrono::steady_clock::now() +
			      std::chrono::duration<double>(timeout_s);
	for (unsigned i = 0; i < num_lanes; i++) {
		enum doca_ctx_states cs;
		for (;;) {
			doca_ctx_get_state(lanes[i].ctx, &cs);
			if (cs == DOCA_CTX_STATE_RUNNING) break;
			if (std::chrono::steady_clock::now() > deadline)
				FAIL("dpa rdma ctx did not reach RUNNING in " << timeout_s << " s");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	launch_kernels();
}

/* seq goes last so the kernel never reads a half-written descriptor. Spinning on a full ring is the
 * backpressure DOCA_ERROR_FULL gives the proxy on the CPU data path.
 *
 * The kernel drains strictly in order, so a thread that publishes its slot before a lower-numbered
 * one is published just leaves the kernel waiting at the gap; it does not reorder or lose work. */
uint64_t DpaFabric::submit(Lane &l, uint32_t op, unsigned conn_idx, size_t local_off,
			   size_t remote_off, size_t len, uint32_t imm, uint64_t wr_id)
{
	const uint64_t mine = l.sub_tail.fetch_add(1, std::memory_order_relaxed);
	while (inv_line(l.sub_consumed),
	       mine - __atomic_load_n(l.sub_consumed, __ATOMIC_ACQUIRE) >= ring_len)
		asm volatile("yield" ::: "memory");

	submit_slot *d = &l.sub[mine % ring_len];
	d->wr_id = wr_id;
	d->local_off = local_off;
	d->remote_off = remote_off;
	d->len = (uint32_t)len;
	d->imm = imm;
	d->conn = conn_idx;
	d->op = op;
	__atomic_store_n(&d->seq, mine + 1, __ATOMIC_RELEASE);
	flush_line(d);
	l.posted.fetch_add(1, std::memory_order_relaxed);
	return mine + 1;
}

void DpaFabric::send_imm(unsigned lane, unsigned conn_idx, uint32_t imm, size_t offset, size_t len,
			 uint64_t wr_id)
{
	submit(lane_of(lane), OP_SEND, conn_idx, offset, 0, len, htobe32(imm), wr_id);
}

/* Blocks until the DPA has actually posted it. DocaRdma::post_recv is synchronous, and buddy relies
 * on that: it posts its receives during setup and starts sending with no barrier in between, so a
 * receive that is merely queued lets the peer's send arrive first and fail with RECV_ERR. */
void DpaFabric::post_recv(unsigned lane, size_t offset, size_t len, uint64_t wr_id)
{
	Lane &l = lane_of(lane);
	const uint64_t seq = submit(l, OP_RECV, 0, offset, 0, len, 0, wr_id);
	while (inv_line(l.sub_consumed), __atomic_load_n(l.sub_consumed, __ATOMIC_ACQUIRE) < seq)
		asm volatile("yield" ::: "memory");
}

void DpaFabric::read(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off,
		     size_t len, uint64_t wr_id)
{
	submit(lane_of(lane), OP_READ, conn_idx, local_off, remote_off, len, 0, wr_id);
}

void DpaFabric::write(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off,
		      size_t len, uint64_t wr_id)
{
	submit(lane_of(lane), OP_WRITE, conn_idx, local_off, remote_off, len, 0, wr_id);
}

void DpaFabric::write_imm(unsigned lane, unsigned conn_idx, size_t local_off, size_t remote_off,
			  size_t len, uint32_t imm, uint64_t wr_id)
{
	submit(lane_of(lane), OP_WRITE, conn_idx, local_off, remote_off, len, htobe32(imm), wr_id);
}

/* A slot is ready once its seq passes the head; the kernel writes seq last. The compare-exchange
 * claims the slot, so two threads polling the same lane never take the same completion. */
bool DpaFabric::poll(unsigned lane, completion *c)
{
	Lane &l = lane_of(lane);
	for (;;) {
		uint64_t head = l.ring_head.load(std::memory_order_relaxed);
		ring_slot *s = &l.ring[head % ring_len];
		inv_line(s);
		if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != head + 1) return false;
		c->wr_id = s->wr_id;
		c->imm = be32toh(s->imm);
		c->len = s->len;
		c->conn = s->conn;
		c->op = (op_type)s->op;
		if (l.ring_head.compare_exchange_weak(head, head + 1, std::memory_order_acq_rel,
						      std::memory_order_relaxed)) {
			/* Tell the kernel this slot is free again, or it wraps and overwrites. */
			__atomic_store_n(l.ring_consumed, head + 1, __ATOMIC_RELEASE);
			flush_line(l.ring_consumed);
			l.completed.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
	}
}

bool DpaFabric::wait_idle(unsigned lane, double timeout_s)
{
	Lane &l = lane_of(lane);
	const auto deadline = std::chrono::steady_clock::now() +
			      std::chrono::duration<double>(timeout_s);
	while (l.completed < l.posted) {
		completion c;
		if (poll(lane, &c)) continue;
		if (std::chrono::steady_clock::now() > deadline) return false;
	}
	return true;
}

} // namespace buddy::rdma
