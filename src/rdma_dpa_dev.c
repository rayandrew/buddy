/* Kernel half of DpaFabric. Structs must match rdma_dpa.h field for field.
 *
 * Arm memory is not addressable from here. Every ring access goes through
 * doca_dpa_dev_mmap_get_external_ptr, whose mmap and handle must come from the PF context, and
 * loads and stores need explicit window invalidate/writeback to cross the boundary. Slots are
 * padded to 64 bytes because that window is 64B aligned.
 *
 * The DPA data path has no per-request wr_id (doca_dpa_dev_get_completion_user_data is a uint32
 * fixed at attach time), so send wr_ids come from a per-connection FIFO: RC completions are
 * ordered, so the Nth completion on a connection belongs to the Nth descriptor posted on it.
 */
#include <doca_dpa_dev.h>
#include <doca_dpa_dev_buf.h>
#include <doca_dpa_dev_rdma.h>
#include <dpaintrin.h>

#define MAX_CONNS 8
#define WRID_FIFO 1024

enum { OP_SEND, OP_RECV, OP_READ, OP_WRITE };

struct submit_slot {
	uint64_t wr_id;
	uint64_t local_off;
	uint64_t remote_off;
	uint32_t len;
	uint32_t imm;
	uint32_t conn;
	uint32_t op;
	uint64_t seq;
	uint64_t pad[2];
};

struct ring_slot {
	uint64_t wr_id;
	uint32_t imm;
	uint32_t len;
	uint32_t conn;
	uint32_t op;
	uint64_t seq;
	uint64_t pad[4];
};

#define ARG_MAGIC 0x0DFAB0FEEDULL

struct fabric_arg {
	uint64_t magic;         /* guards against a host/device layout drift going unnoticed */
	doca_dpa_dev_t ctx;
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
	uint64_t remote_mmap[MAX_CONNS];
	uint64_t remote_base[MAX_CONNS];
	uint32_t num_conns;

	/* Device-side only, never copied across the host boundary: a doca_dpa h2d/d2h round trip
	 * silently corrupts at 64 KB while still returning DOCA_SUCCESS, and the FIFOs alone are
	 * that big. Keeping this struct small is what makes the copy honest. */
	uint64_t wrid_addr;
	uint64_t wrid_head[MAX_CONNS];
	uint64_t wrid_tail[MAX_CONNS];

	/* Receives need their own ids: the proxy indexes its landing slot by the completion's wr_id,
	 * so publishing 0 makes every message look like it arrived in slot 0. The receive queue is
	 * FIFO, so the Nth receive completion belongs to the Nth posted receive. */
	uint64_t rwrid_addr;
	uint64_t rwrid_head;
	uint64_t rwrid_tail;

	uint64_t rack;          /* BUDDY_DPA_RACK: call receive_ack per drained receive */
	uint64_t always_flush;
	uint64_t errors;
	uint64_t last_err;
	uint64_t stop_addr;     /* device word the Arm sets to break the loop at teardown */
	uint64_t notify;        /* this thread's own notification handle, rung to re-trigger itself */
	uint64_t minimal;
	uint64_t wakes;
};

/* flags carries FLUSH on the last post of a batch. Without it the context aggregates the work and
 * never rings it to hardware, so the ops are accepted and simply never complete. */
static void post_one(struct fabric_arg *a, struct submit_slot *d, uint32_t flags)
{
	const uint32_t c = d->conn;
	const uint64_t laddr = a->local_base + d->local_off;
	const uint64_t raddr = a->remote_base[c] + d->remote_off;

	switch (d->op) {
	case OP_RECV:
		doca_dpa_dev_rdma_post_receive(a->rdma, a->local_mmap, laddr, d->len);
		((uint64_t *)a->rwrid_addr)[a->rwrid_tail % WRID_FIFO] = d->wr_id;
		a->rwrid_tail++;
		return;
	case OP_SEND:
		doca_dpa_dev_rdma_post_send_imm(a->rdma, c, a->local_mmap, laddr, d->len, d->imm,
						flags);
		break;
	case OP_READ:
		doca_dpa_dev_rdma_post_read(a->rdma, c, a->local_mmap, laddr,
					    a->remote_mmap[c], raddr, d->len, flags);
		break;
	case OP_WRITE:
		doca_dpa_dev_rdma_post_write_imm(a->rdma, c, a->remote_mmap[c], raddr,
						 a->local_mmap, laddr, d->len, d->imm, flags);
		break;
	}
	((uint64_t *)a->wrid_addr)[c * WRID_FIFO + (a->wrid_tail[c] % WRID_FIFO)] = d->wr_id;
	a->wrid_tail[c]++;
}

static void drain_submits(struct fabric_arg *a)
{
	struct submit_slot *sub =
		(struct submit_slot *)doca_dpa_dev_mmap_get_external_ptr(a->sub_mmap, a->sub_addr);
	uint64_t *consumed =
		(uint64_t *)doca_dpa_dev_mmap_get_external_ptr(a->sub_mmap, a->consumed_addr);
	uint64_t posted = 0;

	for (;;) {
		struct submit_slot *d = sub + (a->sub_head % a->sub_len);
		struct submit_slot *next;
		/* Invalidate per iteration, not once per pass: the Arm writes into this window while the
		 * loop runs, and a stale read here silently drops the submit rather than deferring it. */
		__dpa_thread_window_read_inv();
		if (d->seq != a->sub_head + 1) break;
		next = sub + ((a->sub_head + 1) % a->sub_len);
		/* Defer the flush only when the next op can carry it. post_receive takes no flags, so a
		 * batch ending in a receive would leave the preceding send queued and never rung: the
		 * send never completes and the peer never sees it. */
		post_one(a, d,
			 (!a->always_flush && next->seq == a->sub_head + 2 && next->op != OP_RECV)
				 ? DOCA_DPA_DEV_SUBMIT_FLAG_NONE
				 : DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
		a->sub_head++;
		posted++;
	}
	if (posted) {
		*consumed = a->sub_head;
		__dpa_thread_window_writeback();
	}
}

/* seq is written last so the Arm never reads a half-written slot. */
static void publish(struct ring_slot *ring, struct fabric_arg *a, uint64_t wr_id, uint32_t imm,
		    uint32_t len, uint32_t conn, uint32_t op)
{
	struct ring_slot *s = ring + (a->tail % a->ring_len);
	s->wr_id = wr_id;
	s->imm = imm;
	s->len = len;
	s->conn = conn;
	s->op = op;
	/* One writeback: a slot is a single 64B line, so payload and seq leave together and the Arm
	 * cannot observe a new seq against a stale payload. */
	s->seq = ++a->tail;
	__dpa_thread_window_writeback();
}

static void drain_completions(struct fabric_arg *a)
{
	doca_dpa_dev_completion_element_t e;
	struct ring_slot *ring = NULL;
	volatile uint64_t *consumed = NULL;
	uint32_t acked = 0;
	uint32_t recvs = 0;

	/* Read the Arm's index once, before any publish. Invalidating the window inside the loop would
	 * discard publish()'s pending writes. */
	consumed = (volatile uint64_t *)doca_dpa_dev_mmap_get_external_ptr(a->ring_mmap,
									   a->ring_consumed_addr);
	__dpa_thread_window_read_inv();
	const uint64_t free_until = *consumed + a->ring_len;
	ring = (struct ring_slot *)doca_dpa_dev_mmap_get_external_ptr(a->ring_mmap, a->ring_addr);

	/* Space is checked before fetching, never after: doca_dpa_dev_get_completion consumes the
	 * element, so breaking out once it has been fetched loses that completion outright. */
	while (a->tail < free_until && doca_dpa_dev_get_completion(a->comp, &e)) {
		const doca_dpa_dev_completion_type_t t = doca_dpa_dev_get_completion_type(e);
		const uint32_t imm = doca_dpa_dev_get_completion_immediate(e);
		const uint32_t c = 0;

		switch (t) {
		case DOCA_DPA_DEV_COMP_RECV_RDMA_WRITE_IMM:
		case DOCA_DPA_DEV_COMP_RECV_SEND:
		case DOCA_DPA_DEV_COMP_RECV_SEND_IMM: {
			uint64_t rid = 0;
			if (a->rwrid_head != a->rwrid_tail)
				rid = ((uint64_t *)a->rwrid_addr)[a->rwrid_head++ % WRID_FIFO];
			publish(ring, a, rid, imm, 0, c, OP_RECV);
			recvs++;
			break;
		}
		case DOCA_DPA_DEV_COMP_SEND: {
			uint64_t wr_id = 0;
			if (a->wrid_head[c] != a->wrid_tail[c])
				wr_id = ((uint64_t *)a->wrid_addr)
					[c * WRID_FIFO + (a->wrid_head[c]++ % WRID_FIFO)];
			publish(ring, a, wr_id, imm, 0, c, OP_SEND);
			break;
		}
		default:
			/* SEND_ERR / RECV_ERR. Counting them is the only way they are visible: an
			 * error completion carries no error text to the Arm. */
			a->errors++;
			a->last_err = t;
			if (t == DOCA_DPA_DEV_COMP_RECV_ERR) {
				recvs++;
				if (a->rwrid_head != a->rwrid_tail) a->rwrid_head++;
			} else if (a->wrid_head[c] != a->wrid_tail[c]) {
				a->wrid_head[c]++;
			}
			break;
		}
		acked++;
	}
	if (recvs && a->rack) doca_dpa_dev_rdma_receive_ack(a->rdma, recvs);
	if (acked) doca_dpa_dev_completion_ack(a->comp, acked);
}

/* Launched once by the host with doca_dpa_kernel_launch_update_set and runs until the Arm sets the
 * stop word. A doca_dpa_thread is deliberately not used: a thread only runs when its completion
 * context receives something and the Arm has no way to wake one (doca_dpa_dev_thread_notify is
 * documented as not relevant to host-launched kernels), so a thread can never notice a submit.
 *
 * Holding an execution unit is the intent: this is buddy's poll loop, moved off the Arm. */
__dpa_global__ void fabric_kernel(uint64_t arg_addr)
{
	struct fabric_arg *a = (struct fabric_arg *)arg_addr;
	volatile uint64_t *stop;

	/* Distinct markers so the host can tell a kernel that never ran (0) from one that ran and
	 * found a layout mismatch. */
	if (a->magic != ARG_MAGIC) {
		a->wakes = 0xBADF00DULL;
		return;
	}
	a->wakes = 1;

	stop = (volatile uint64_t *)a->stop_addr;
	if (a->ctx) doca_dpa_dev_device_set(a->ctx);

	while (!*stop) {
		drain_submits(a);
		drain_completions(a);
		a->wakes++;
	}
}
