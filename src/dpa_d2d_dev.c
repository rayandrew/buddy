/* DPA-native D2D engine: receive, route and forward without waking the Arm.
 *
 * Everything here reaches its buffers with an ordinary load, because they are DPA memory registered
 * for RDMA with doca_mmap_set_dpa_memrange. There is no window, no cache maintenance, and no ring
 * shared with the Arm. See DESIGN-DPA.md for why the first version had all three.
 *
 * One engine drives one thread. The receive pool is split across engines because the record walk is
 * serial inside a buffer but independent across buffers.
 */
#include <doca_dpa_dev.h>
#include <doca_dpa_dev_buf.h>
#include <doca_dpa_dev_rdma.h>

#define D2D_MAX_PEERS 8
#define D2D_MAX_RANKS 64
#define D2D_PENDING 8192
#define D2D_MAX_BUFS 512
/* Completions handled per drain pass before the handler must return to the scheduler. */
#define D2D_DRAIN_MAX 8

/* Mirrors buddy::request_head. Records are packed head to tail with no padding. */
struct request_head {
	uint32_t size;
	int32_t dst;
};

#define D2D_MAGIC 0x0D2D0FEEDULL

struct d2d_engine {
	uint64_t magic;         /* guards against host and device layouts drifting apart */
	doca_dpa_dev_t ctx;
	doca_dpa_dev_completion_t comp;
	doca_dpa_dev_mmap_t mmap;

	/* One RDMA context per peer, all attached to this engine's completion. */
	doca_dpa_dev_rdma_t rdma[D2D_MAX_PEERS];
	uint32_t num_peers;

	uint64_t rx_base;       /* nrx receive buffers owned by this engine */
	uint64_t buf_size;
	uint32_t nrx;

	/* rank -> peer index, or D2D_LOCAL for a rank served by this node. */
	uint8_t route[D2D_MAX_RANKS];
	uint32_t num_ranks;

	/* A received buffer is re-posted only after every forward taken from it has completed, so a
	 * forward can read it in place instead of copying. Send completions arrive in order on a
	 * reliable connection, so a FIFO of source buffers is enough to match them up. */
	/* Its own device allocation, never copied across the host boundary: a big argument block makes
	 * the h2d and d2h copies unreliable, and the counters live past it. */
	uint64_t pending_addr;
	uint32_t pend_head;
	uint32_t pend_tail;
	uint32_t refcount[D2D_MAX_BUFS];

	/* Which buffer each posted receive used, in posting order. The completion's wr_index cannot be
	 * turned into a buffer index arithmetically: that only holds while receives are re-posted in
	 * completion order, which stops being true the moment a buffer is held for a forward. */
	uint64_t rxfifo_addr;
	uint32_t rx_head;
	uint32_t rx_tail;

	/* Load generation, so the device can drive itself for a measurement instead of being fed by
	 * the Arm. tx_len is zero when this engine only routes. */
	uint64_t tx_base;
	uint64_t tx_len;
	uint32_t ntx;
	uint32_t next_tx;

	uint64_t wakes;         /* handler entries, so a silent engine is distinguishable from an idle one */
	uint64_t tx_msgs;
	uint64_t rx_msgs;
	uint64_t records;
	uint64_t forwards;
	uint64_t local;
	uint64_t bad_records;
	uint64_t errors;
	uint64_t last_err;
	uint64_t first_err_rx;  /* rx_msgs when the first error hit */
	uint64_t first_err_tx;
};

#define D2D_LOCAL 0xFF
/* Marks a pending send as generated rather than forwarded, so its completion re-arms the generator
 * instead of releasing a receive buffer. Both kinds share a queue pair and complete in order. */
#define D2D_GEN 0xFFFFFFFFu

static void repost(struct d2d_engine *e, uint32_t idx)
{
	((uint32_t *)e->rxfifo_addr)[e->rx_tail++ % D2D_PENDING] = idx;
	doca_dpa_dev_rdma_post_receive(e->rdma[0], e->mmap,
				       e->rx_base + (uint64_t)idx * e->buf_size, e->buf_size);
}

/* Forward one byte range to a peer straight out of the buffer it arrived in. */
static void forward(struct d2d_engine *e, uint32_t peer, uint32_t idx, uint64_t off, uint64_t len)
{
	e->refcount[idx]++;
	((uint32_t *)e->pending_addr)[e->pend_tail++ % D2D_PENDING] = idx;
	doca_dpa_dev_rdma_post_send_imm(e->rdma[peer], 0, e->mmap,
					e->rx_base + (uint64_t)idx * e->buf_size + off, len, 0,
					DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
	e->forwards++;
}

/* Walk the records and forward each run of consecutive records sharing a destination as one send.
 * Runs matter: buddy aggregates many small requests per buffer, and one send per record would cost
 * far more than the walk itself. */
static void route_buffer(struct d2d_engine *e, uint32_t idx, uint64_t len)
{
	const uint64_t buf = e->rx_base + (uint64_t)idx * e->buf_size;
	uint64_t pos = 0, run_start = 0;
	uint32_t run_peer = D2D_LOCAL;
	int have_run = 0;

	while (pos + sizeof(struct request_head) <= len) {
		const struct request_head *h = (const struct request_head *)(buf + pos);
		const uint64_t rec = sizeof(*h) + h->size;
		uint32_t peer;

		if (h->size == 0 || pos + rec > len) {
			e->bad_records++;
			break;
		}
		e->records++;

		peer = (h->dst >= 0 && (uint32_t)h->dst < e->num_ranks) ? e->route[h->dst]
									: D2D_LOCAL;
		if (have_run && peer != run_peer) {
			if (run_peer != D2D_LOCAL)
				forward(e, run_peer, idx, run_start, pos - run_start);
			else
				e->local++;
			have_run = 0;
		}
		if (!have_run) {
			run_start = pos;
			run_peer = peer;
			have_run = 1;
		}
		pos += rec;
	}

	if (have_run) {
		if (run_peer != D2D_LOCAL)
			forward(e, run_peer, idx, run_start, pos - run_start);
		else
			e->local++;
	}

	/* Nothing was forwarded out of it, so it is free immediately. */
	if (e->refcount[idx] == 0) repost(e, idx);
}

static void post_generated(struct d2d_engine *e)
{
	const uint32_t s = e->next_tx++ % e->ntx;
	e->tx_msgs++;
	((uint32_t *)e->pending_addr)[e->pend_tail++ % D2D_PENDING] = D2D_GEN;
	doca_dpa_dev_rdma_post_send_imm(e->rdma[0], 0, e->mmap,
					e->tx_base + (uint64_t)s * e->buf_size, e->tx_len, 0,
					DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
}

__dpa_rpc__ uint64_t d2d_generate(uint64_t arg, uint64_t window, uint64_t len)
{
	struct d2d_engine *e = (struct d2d_engine *)arg;
	if (e->ctx) doca_dpa_dev_device_set(e->ctx);
	e->tx_len = len;
	for (uint64_t i = 0; i < window; i++) post_generated(e);
	return 0;
}

/* Posts this engine's receives. Only the device can post once the data path is on the DPA. */
__dpa_rpc__ uint64_t d2d_arm(uint64_t arg)
{
	struct d2d_engine *e = (struct d2d_engine *)arg;
	if (e->ctx) doca_dpa_dev_device_set(e->ctx);
	for (uint32_t i = 0; i < e->nrx; i++) repost(e, i);
	doca_dpa_dev_completion_request_notification(e->comp);
	return 0;
}

__dpa_global__ void d2d_handler(uint64_t arg)
{
	struct d2d_engine *e = (struct d2d_engine *)arg;
	doca_dpa_dev_completion_element_t el;

	if (e->magic != D2D_MAGIC) return;
	e->wakes++;
	if (e->ctx) doca_dpa_dev_device_set(e->ctx);

	/* Bounded, and the bound is load-bearing. Handling a completion posts another operation, whose
	 * completion can arrive before this drain ends, so an unbounded loop never empties under load
	 * and the handler never reaches the reschedule below: measured as the engine waking exactly
	 * once and then going silent at a window of 64. */
	for (uint32_t round = 0; round < 1; round++) {
		uint32_t drained = 0;

		while (drained < D2D_DRAIN_MAX && doca_dpa_dev_get_completion(e->comp, &el)) {
			const doca_dpa_dev_completion_type_t t =
				doca_dpa_dev_get_completion_type(el);
			/* One at a time: acking a whole drain in one call frees nothing and the queue
			 * fills exactly once. Measured, see DESIGN-DPA.md. */
			doca_dpa_dev_completion_ack(e->comp, 1);
			drained++;

			switch (t) {
			case DOCA_DPA_DEV_COMP_RECV_SEND:
			case DOCA_DPA_DEV_COMP_RECV_SEND_IMM:
			case DOCA_DPA_DEV_COMP_RECV_RDMA_WRITE_IMM: {
				uint32_t idx;
				if (e->rx_head == e->rx_tail) {
					/* A receive completed that was never posted here. */
					e->errors++;
					e->last_err = t;
					break;
				}
				idx = ((uint32_t *)e->rxfifo_addr)[e->rx_head++ % D2D_PENDING];
				e->rx_msgs++;
				route_buffer(e, idx, e->buf_size);
				break;
			}
			case DOCA_DPA_DEV_COMP_SEND: {
				if (e->pend_head != e->pend_tail) {
					const uint32_t idx =
						((uint32_t *)e->pending_addr)[e->pend_head++ %
								 D2D_PENDING];
					if (idx == D2D_GEN)
						post_generated(e);
					else if (e->refcount[idx] && --e->refcount[idx] == 0)
						repost(e, idx);
				}
				break;
			}
			default:
				/* Record when the first error hit, not just that it did: errors track the
				 * engine count at about one each, so whether they land during the first
				 * few messages or under load decides if this is a start-up race. */
				if (!e->errors) {
					e->first_err_rx = e->rx_msgs;
					e->first_err_tx = e->tx_msgs;
				}
				e->errors++;
				e->last_err = t;
				break;
			}
		}

		doca_dpa_dev_completion_request_notification(e->comp);
		if (!drained) break;
	}

	doca_dpa_dev_thread_reschedule();
}
