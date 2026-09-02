#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <endian.h>
#include <iostream>
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

	uint64_t rack;
	uint64_t always_flush;
	uint64_t errors;
	uint64_t last_err;
	uint64_t stop_addr;
	uint64_t notify;
	uint64_t minimal;
	uint64_t wakes;
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

DpaFabric::DpaFabric(unsigned num_connections, char *mem, size_t mem_len, unsigned lanes)
	: num_connections(num_connections), mem(mem), mem_len(mem_len), ring_len(dpa_ring_slots())
{
	(void)lanes;
	debug = getenv("BUDDY_DPA_DEBUG") != nullptr;
	const uint32_t perms = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
			       DOCA_ACCESS_FLAG_RDMA_WRITE;
	dev = open_dev(dpa_ibdev());
	pf_dev = open_dev(dpa_pf_ibdev());

	CHECK_DOCA(doca_dpa_create(pf_dev, &pf_dpa));
	CHECK_DOCA(doca_dpa_set_app(pf_dpa, buddy_dpa_app));
	CHECK_DOCA(doca_dpa_start(pf_dpa));
	if (dev != pf_dev)
		CHECK_DOCA(doca_dpa_device_extend(pf_dpa, dev, &dpa));
	else
		dpa = pf_dpa;

	/* Both rings are registered on the PF: doca_dpa_dev_mmap_get_external_ptr only works for an
	 * mmap and handle from the PF context, not the extended one. */
	/* 64B-aligned: the DPA reaches these through a window with a 64B alignment restriction, and a
	 * misaligned range makes the kernel's writes invisible to the Arm with no error anywhere. */
	/* ring_consumed shares the mapping but its own cache line: without it the kernel would wrap and
	 * overwrite completions the proxy has not read yet, losing them silently. */
	const size_t ring_bytes = kLineSize + sizeof(ring_slot) * ring_len;
	ring_mem = (char *)aligned_alloc(kLineSize, ring_bytes);
	if (!ring_mem) FAIL("dpa ring allocation failed");
	memset(ring_mem, 0, ring_bytes);
	ring_consumed = (uint64_t *)ring_mem;
	ring = (ring_slot *)(ring_mem + kLineSize);
	CHECK_DOCA(doca_mmap_create(&ring_mmap));
	CHECK_DOCA(doca_mmap_add_dev(ring_mmap, pf_dev));
	CHECK_DOCA(doca_mmap_set_permissions(ring_mmap, perms));
	CHECK_DOCA(doca_mmap_set_memrange(ring_mmap, ring_mem, ring_bytes));
	CHECK_DOCA(doca_mmap_start(ring_mmap));

	/* sub_consumed shares the mapping but its own cache line. */
	sub_mem = (char *)aligned_alloc(kLineSize, kLineSize + sizeof(submit_slot) * ring_len);
	if (!sub_mem) FAIL("dpa submit ring allocation failed");
	memset(sub_mem, 0, kLineSize + sizeof(submit_slot) * ring_len);
	sub_consumed = (uint64_t *)sub_mem;
	sub = (submit_slot *)(sub_mem + kLineSize);
	CHECK_DOCA(doca_mmap_create(&sub_mmap));
	CHECK_DOCA(doca_mmap_add_dev(sub_mmap, pf_dev));
	CHECK_DOCA(doca_mmap_set_permissions(sub_mmap, perms));
	CHECK_DOCA(doca_mmap_set_memrange(sub_mmap, sub_mem,
					  kLineSize + sizeof(submit_slot) * ring_len));
	CHECK_DOCA(doca_mmap_start(sub_mmap));

	CHECK_DOCA(doca_mmap_create(&mmap));
	CHECK_DOCA(doca_mmap_add_dev(mmap, dev));
	CHECK_DOCA(doca_mmap_set_permissions(mmap, perms));
	CHECK_DOCA(doca_mmap_set_memrange(mmap, mem, mem_len));
	CHECK_DOCA(doca_mmap_start(mmap));

	/* The thread reads its local storage while starting, so the argument block comes first. */
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(fabric_arg), &arg_dev));
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint64_t), &stop_dev));
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint64_t) * kMaxConns * kWridFifo, &wrid_dev));
	CHECK_DOCA(doca_dpa_memset(dpa, wrid_dev, 0, sizeof(uint64_t) * kMaxConns * kWridFifo));
	CHECK_DOCA(doca_dpa_mem_alloc(dpa, sizeof(uint64_t) * kWridFifo, &rwrid_dev));
	CHECK_DOCA(doca_dpa_memset(dpa, rwrid_dev, 0, sizeof(uint64_t) * kWridFifo));
	{
		const uint64_t zero = 0;
		CHECK_DOCA(doca_dpa_h2d_memcpy(dpa, stop_dev, (void *)&zero, sizeof(zero)));
	}
	/* A completion context still needs a thread object to attach to, but that thread is never
	 * run: the data path is the launched kernel below, not a thread activation. */
	CHECK_DOCA(doca_dpa_thread_create(dpa, &thread));
	CHECK_DOCA(doca_dpa_thread_set_func_arg(thread, &fabric_kernel, 0));
	CHECK_DOCA(doca_dpa_thread_start(thread));

	CHECK_DOCA(doca_dpa_completion_create(dpa, 4096, &dpa_comp));
	CHECK_DOCA(doca_dpa_completion_set_thread(dpa_comp, thread));
	CHECK_DOCA(doca_dpa_completion_start(dpa_comp));

	CHECK_DOCA(doca_rdma_create(dev, &rdma));
	ctx = doca_rdma_as_ctx(rdma);
	CHECK_DOCA(doca_ctx_set_datapath_on_dpa(ctx, dpa));
	CHECK_DOCA(doca_rdma_set_permissions(rdma, perms));
	CHECK_DOCA(doca_rdma_set_grh_enabled(rdma, 1));
	/* Same GID as the other legs. Left unset the connection still establishes and the data lands
	 * wrong, which surfaces only as a RECV_ERR completion. */
	CHECK_DOCA(doca_rdma_set_gid_index(rdma, dpa_gid()));
	CHECK_DOCA(doca_rdma_set_max_num_connections(rdma, num_connections));
	CHECK_DOCA(doca_rdma_set_rnr_retry_count(rdma, 7));
	/* buddy sends d2d_size-16 (4080 B by default). Measured: the DPA path completes 1 KB sends and
	 * silently drops 4080 B ones. Raising the MTU is not permitted here, so the ceiling is
	 * whatever the device reports; the check below turns exceeding it into a clear failure. */
	{
		const doca_error_t mtu = doca_rdma_set_mtu(rdma, DOCA_MTU_SIZE_4K_BYTES);
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
		CHECK_DOCA(doca_rdma_set_send_queue_size(rdma, std::min<uint32_t>(buddy::queue_depth(), max_sq)));
		const doca_error_t rq = doca_rdma_set_recv_queue_size(rdma, std::min<uint32_t>(buddy::queue_depth(), max_rq));
		if (rq != DOCA_SUCCESS && rq != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(rq));
		/* Measured: at the default list length each post_receive reserves four receive-queue
		 * entries, so only one receive in four is usable. buddy chains no buffers. */
		const doca_error_t bl = doca_rdma_task_receive_set_dst_buf_list_len(rdma, 1);
		if (bl != DOCA_SUCCESS && bl != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(bl));
		CHECK_DOCA(doca_rdma_cap_get_max_message_size(info, &max_msg));
		if (debug)
			std::cerr << "[dpa] max_sq=" << max_sq << " max_rq=" << max_rq
				  << " requested=" << buddy::queue_depth()
				  << " set_rq=" << doca_error_get_name(rq)
				  << " set_buf_list=" << doca_error_get_name(bl) << std::endl;
	}
	CHECK_DOCA(doca_rdma_dpa_completion_attach(rdma, dpa_comp));
	CHECK_DOCA(doca_ctx_start(ctx));
	CHECK_DOCA(doca_rdma_get_dpa_handle(rdma, &dpa_rdma_handle));

	if (num_connections > kMaxConns) FAIL("dpa fabric supports at most " << kMaxConns << " peers");
	conns = new doca_rdma_connection *[num_connections]();
	remote_mmap = new doca_mmap *[num_connections]();
}

/* The kernel cannot run until every peer's mmap handle is known, so the argument block is filled
 * and the thread started only after all connections are up. */
void DpaFabric::start_kernel()
{
	doca_dpa_dev_t dpa_handle = 0;
	CHECK_DOCA(doca_dpa_get_dpa_handle(dpa, &dpa_handle));
	uint64_t comp_handle = 0;
	CHECK_DOCA(doca_dpa_completion_get_dpa_handle(dpa_comp, &comp_handle));

	fabric_arg a = {};
	a.magic = kArgMagic;
	a.ctx = dpa_handle;
	a.comp = comp_handle;
	a.rdma = dpa_rdma_handle;
	CHECK_DOCA(doca_mmap_dev_get_dpa_handle(sub_mmap, pf_dev, (doca_dpa_dev_mmap_t *)&a.sub_mmap));
	a.sub_addr = (uint64_t)sub;
	a.sub_len = ring_len;
	a.consumed_addr = (uint64_t)sub_consumed;
	CHECK_DOCA(doca_mmap_dev_get_dpa_handle(ring_mmap, pf_dev,
						(doca_dpa_dev_mmap_t *)&a.ring_mmap));
	a.ring_addr = (uint64_t)ring;
	a.ring_len = ring_len;
	a.ring_consumed_addr = (uint64_t)ring_consumed;
	a.stop_addr = stop_dev;
	a.wrid_addr = wrid_dev;
	a.rwrid_addr = rwrid_dev;
	/* Off by default: acking once per receive completion caps progress at the initially posted
	 * receive count, while never acking runs 30x further. The API reads as incremental but does
	 * not behave that way here. */
	a.rack = getenv("BUDDY_DPA_RACK") ? atoi(getenv("BUDDY_DPA_RACK")) : 0;
	a.always_flush = getenv("BUDDY_DPA_FLUSH_ALL") ? 1 : 0;
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

	CHECK_DOCA(doca_dpa_h2d_memcpy(dpa, arg_dev, &a, sizeof(a)));

	/* Asynchronous: the call returns once the launch is submitted, and the kernel then owns the
	 * data path until the stop word is set. */
	CHECK_DOCA(doca_dpa_kernel_launch_update_set(dpa, NULL, 0, NULL, 0, 1, &fabric_kernel,
						     arg_dev));

	/* BUDDY_DPA_DEBUG=1 reads the kernel's own counters back, which is the only way to tell a
	 * thread that never ran from one that runs but consumes nothing. */
	if (getenv("BUDDY_DPA_DEBUG")) {
		std::thread([this] {
			for (;;) {
				fabric_arg d = {};
				if (doca_dpa_d2h_memcpy(dpa, &d, arg_dev, sizeof(d)) != DOCA_SUCCESS)
					return;
				std::cerr << "[dpa] magic=" << std::hex << d.magic << std::dec
					  << " err=" << d.errors << "/" << std::hex << d.last_err
					  << std::dec << " wakes=" << d.wakes
					  << " sub_head=" << d.sub_head
					  << " tail=" << d.tail << " wrid_tail=" << d.wrid_tail[0]
					  << " | host sub_tail=" << sub_tail.load()
					  << " ring_head=" << ring_head.load()
					  << " consumed=" << *sub_consumed << std::endl;
				std::this_thread::sleep_for(std::chrono::seconds(2));
			}
		}).detach();
	}
}

DpaFabric::~DpaFabric()
{
	/* Release the spinning kernel's EU before tearing down anything it touches. */
	if (stop_dev) {
		const uint64_t one = 1;
		doca_dpa_h2d_memcpy(dpa, stop_dev, (void *)&one, sizeof(one));
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	doca_ctx_stop(ctx);
	doca_rdma_destroy(rdma);
	doca_dpa_completion_destroy(dpa_comp);
	doca_dpa_thread_destroy(thread);
	doca_mmap_destroy(ring_mmap);
	doca_mmap_destroy(mmap);
	for (unsigned i = 0; i < num_connections; i++)
		if (remote_mmap[i]) doca_mmap_destroy(remote_mmap[i]);
	if (dpa != pf_dpa) doca_dpa_destroy(dpa);
	doca_dpa_destroy(pf_dpa);
	if (pf_dev != dev) doca_dev_close(pf_dev);
	doca_dev_close(dev);
	delete[] conns;
	doca_mmap_destroy(sub_mmap);
	delete[] remote_mmap;
	free(ring_mem);
	free(sub_mem);
}

void DpaFabric::connect(unsigned idx, int sock, bool is_server)
{
	const void *blob;
	size_t blob_len;
	CHECK_DOCA(doca_rdma_export(rdma, &blob, &blob_len, &conns[idx]));

	uint64_t bl = blob_len;
	full_write(sock, (char *)&bl, sizeof(bl));
	full_write(sock, (char *)blob, blob_len);
	uint64_t pbl;
	full_read(sock, (char *)&pbl, sizeof(pbl));
	char *pblob = new char[pbl];
	full_read(sock, pblob, pbl);
	CHECK_DOCA(doca_rdma_connect(rdma, pblob, pbl, conns[idx]));
	delete[] pblob;

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
	enum doca_ctx_states cs;
	const auto deadline = std::chrono::steady_clock::now() +
			      std::chrono::duration<double>(timeout_s);
	for (;;) {
		doca_ctx_get_state(ctx, &cs);
		if (cs == DOCA_CTX_STATE_RUNNING) { start_kernel(); return; }
		if (std::chrono::steady_clock::now() > deadline)
			FAIL("dpa rdma ctx did not reach RUNNING in " << timeout_s << " s");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

/* seq goes last so the kernel never reads a half-written descriptor. Spinning on a full ring is the
 * backpressure DOCA_ERROR_FULL gives the proxy on the CPU data path.
 *
 * The kernel drains strictly in order, so a thread that publishes its slot before a lower-numbered
 * one is published just leaves the kernel waiting at the gap; it does not reorder or lose work. */
uint64_t DpaFabric::submit(uint32_t op, unsigned conn_idx, size_t local_off, size_t remote_off,
			   size_t len, uint32_t imm, uint64_t wr_id)
{
	const uint64_t mine = sub_tail.fetch_add(1, std::memory_order_relaxed);
	while (inv_line(sub_consumed), mine - __atomic_load_n(sub_consumed, __ATOMIC_ACQUIRE) >= ring_len)
		asm volatile("yield" ::: "memory");

	submit_slot *d = &sub[mine % ring_len];
	d->wr_id = wr_id;
	d->local_off = local_off;
	d->remote_off = remote_off;
	d->len = (uint32_t)len;
	d->imm = imm;
	d->conn = conn_idx;
	d->op = op;
	__atomic_store_n(&d->seq, mine + 1, __ATOMIC_RELEASE);
	flush_line(d);
	posted.fetch_add(1, std::memory_order_relaxed);

	if (debug && mine < 16) {
		static const char *kOp[] = {"SEND", "RECV", "READ", "WRITE"};
		std::cerr << "[dpa] submit#" << mine << " " << kOp[op & 3] << " conn=" << conn_idx
			  << " loff=" << local_off << " roff=" << remote_off << " len=" << len
			  << " imm=0x" << std::hex << be32toh(imm) << std::dec << " wr=" << wr_id
			  << " (mem_len=" << mem_len << ")" << std::endl;
	}
	return mine + 1;
}

void DpaFabric::send_imm(unsigned, unsigned conn_idx, uint32_t imm, size_t offset, size_t len,
			 uint64_t wr_id)
{
	submit(OP_SEND, conn_idx, offset, 0, len, htobe32(imm), wr_id);
}

/* Blocks until the DPA has actually posted it. DocaRdma::post_recv is synchronous, and buddy relies
 * on that: it posts its receives during setup and starts sending with no barrier in between, so a
 * receive that is merely queued lets the peer's send arrive first and fail with RECV_ERR. */
void DpaFabric::post_recv(unsigned, size_t offset, size_t len, uint64_t wr_id)
{
	const uint64_t seq = submit(OP_RECV, 0, offset, 0, len, 0, wr_id);
	while (inv_line(sub_consumed), __atomic_load_n(sub_consumed, __ATOMIC_ACQUIRE) < seq)
		asm volatile("yield" ::: "memory");
}

void DpaFabric::read(unsigned, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len,
		     uint64_t wr_id)
{
	submit(OP_READ, conn_idx, local_off, remote_off, len, 0, wr_id);
}

void DpaFabric::write(unsigned, unsigned conn_idx, size_t local_off, size_t remote_off, size_t len,
		      uint64_t wr_id)
{
	submit(OP_WRITE, conn_idx, local_off, remote_off, len, 0, wr_id);
}

void DpaFabric::write_imm(unsigned, unsigned conn_idx, size_t local_off, size_t remote_off,
			  size_t len, uint32_t imm, uint64_t wr_id)
{
	submit(OP_WRITE, conn_idx, local_off, remote_off, len, htobe32(imm), wr_id);
}

/* A slot is ready once its seq passes the head; the kernel writes seq last. The compare-exchange
 * claims the slot, so two threads polling the same lane never take the same completion. */
bool DpaFabric::poll(unsigned, completion *c)
{
	for (;;) {
		uint64_t head = ring_head.load(std::memory_order_relaxed);
		ring_slot *s = &ring[head % ring_len];
		inv_line(s);
		if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != head + 1) return false;
		c->wr_id = s->wr_id;
		c->imm = be32toh(s->imm);
		c->len = s->len;
		c->conn = s->conn;
		c->op = (op_type)s->op;
		if (ring_head.compare_exchange_weak(head, head + 1, std::memory_order_acq_rel,
						    std::memory_order_relaxed)) {
			/* Tell the kernel this slot is free again, or it wraps and overwrites. */
			__atomic_store_n(ring_consumed, head + 1, __ATOMIC_RELEASE);
			flush_line(ring_consumed);
			const uint64_t n = completed.fetch_add(1, std::memory_order_relaxed);
			if (debug && n < 24) {
				static const char *kOp[] = {"SEND", "RECV", "READ", "WRITE"};
				std::cerr << "[dpa] compl#" << n << " " << kOp[c->op & 3]
					  << " wr=" << c->wr_id << " imm=0x" << std::hex << c->imm
					  << std::dec << " conn=" << c->conn << std::endl;
			}
			return true;
		}
	}
}

bool DpaFabric::wait_idle(unsigned, double timeout_s)
{
	const auto deadline = std::chrono::steady_clock::now() +
			      std::chrono::duration<double>(timeout_s);
	while (completed < posted) {
		completion c;
		if (poll(0, &c)) continue;
		if (std::chrono::steady_clock::now() > deadline) return false;
	}
	return true;
}

} // namespace buddy::rdma
