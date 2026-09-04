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
/* Outstanding sends allowed, well under the 1024 the send queue is created with. Handling one
 * receive can post more than one send, so without a ceiling the queue overruns and the posts stop
 * completing: measured as every counter freezing after about 10k sends. */
#define D2D_SQ_MAX 512
/* Freed buffers per credit ack. One ack per buffer doubles the sends a receive generates, which is
 * what overruns the queue above. */
#define D2D_ACK_BATCH 16
/* Sends one buffer's routing may need before the walk is worth starting. A walk that runs out of
 * capacity half way cannot be retried without re-sending what it already sent. */
#define D2D_ROUTE_MARGIN 4

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

	/* Outbound legs, one per peer index the route table can name. */
	doca_dpa_dev_rdma_t rdma[D2D_MAX_PEERS];
	uint32_t num_peers;

	/* The one leg this engine receives on. Separate from the outbound legs because a completion
	 * does not say which queue pair produced it: an engine that received on more than one could
	 * not tell which buffer had just been filled. One engine per inbound leg instead, each owning
	 * the queue pairs it posts on, so no two threads ever post to the same one. */
	doca_dpa_dev_rdma_t rx_rdma;

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
	uint64_t ack_len;       /* non-zero makes a receiving side acknowledge, freeing its credit */
	uint32_t ntx;
	uint32_t next_tx;

	uint64_t wakes;         /* handler entries, so a silent engine is distinguishable from an idle one */
	uint64_t tx_msgs;
	uint64_t rx_msgs;
	uint64_t records;
	uint64_t forwards;
	uint64_t local;
	uint64_t bad_records;
	uint64_t zerocopy;      /* forwards posted out of the receive buffer, with no copy */
	uint64_t tx_full;       /* forwards dropped because every transmit slot was still in flight */
	uint32_t held;          /* receive buffers held for an outstanding zero-copy forward */
	uint32_t tx_inflight;   /* transmit slots the NIC may still be reading */
	uint32_t freed;         /* buffers freed since the last credit ack */
	/* Buffers received but not yet routed, because there was no room to post their forwards. They
	 * are not re-posted while they sit here, which is the back-pressure that slows the peer: it is
	 * the alternative to dropping their records. Bounded by the same half-the-pool rule as held
	 * buffers, so a receive is always posted and the peer's sends always land. */
	uint64_t defer_addr;
	uint32_t defer_head;
	uint32_t defer_tail;
	uint32_t no_zerocopy;   /* set by the host to force the copy path, for measurement */
	uint32_t gen_paused;    /* generated sends withheld while routing was short of room */
	/* Buffers this engine may park at once. Half the pool when it receives from another router,
	 * which is what stops both of them holding everything and deadlocking; the whole pool bar one
	 * when it receives from a host, which holds no buffers of its own and can simply be stalled. */
	uint32_t defer_max;
	uint64_t errors;
	uint64_t last_err;
	uint64_t first_err_rx;  /* rx_msgs when the first error hit */
	uint64_t first_err_tx;
};

#define D2D_LOCAL 0xFF
/* Marks a pending send as generated rather than forwarded, so its completion re-arms the generator
 * instead of releasing a receive buffer. Both kinds share a queue pair and complete in order. */
#define D2D_GEN 0xFFFFFFFFu
/* An ack carries no staged bytes, so it takes no transmit slot and its completion neither frees one
 * nor re-arms the generator. It still needs a pending entry to keep the FIFO aligned. */
#define D2D_ACK 0xFFFFFFFEu
/* A copy forward frees its transmit slot but must not re-arm the generator: counting it as one
 * inflated the window by every forward ever made, until no slot was ever free again. */
#define D2D_TX 0xFFFFFFFDu

static void repost(struct d2d_engine *e, uint32_t idx)
{
	((uint32_t *)e->rxfifo_addr)[e->rx_tail++ % D2D_PENDING] = idx;
	doca_dpa_dev_rdma_post_receive(e->rx_rdma, e->mmap,
				       e->rx_base + (uint64_t)idx * e->buf_size, e->buf_size);
}

static void copy_bytes(uint64_t dst, uint64_t src, uint64_t len)
{
	uint64_t i = 0;
	for (; i + 8 <= len; i += 8)
		*(uint64_t *)(dst + i) = *(const uint64_t *)(src + i);
	for (; i < len; i++)
		*(uint8_t *)(dst + i) = *(const uint8_t *)(src + i);
}

/* True while a transmit slot is free. Without this the pool wraps and a slot is overwritten while
 * the NIC is still reading it. Only the paths that stage bytes into the transmit pool are counted:
 * a zero-copy forward sends out of its receive buffer and takes no slot. */
static int sq_available(const struct d2d_engine *e)
{
	return (uint32_t)(e->pend_tail - e->pend_head) < D2D_SQ_MAX;
}

static int tx_available(const struct d2d_engine *e)
{
	return e->tx_inflight < e->ntx && sq_available(e);
}

static uint32_t deferred(const struct d2d_engine *e)
{
	return (uint32_t)(e->defer_tail - e->defer_head);
}

/* Room to walk a buffer and post every forward it needs. */
static int route_capacity(const struct d2d_engine *e)
{
	return e->tx_inflight + D2D_ROUTE_MARGIN <= e->ntx &&
	       (uint32_t)(e->pend_tail - e->pend_head) + D2D_ROUTE_MARGIN <= D2D_SQ_MAX;
}

/* Half the pool stays posted as receives, so the peer always has somewhere to deliver and the sends
 * this engine is waiting on always complete. */
static int can_defer(const struct d2d_engine *e)
{
	return deferred(e) + e->held + 1 <= e->defer_max;
}

/* Every user of a transmit slot marks its pending entry the same way, so the completion path gives
 * the slot back without needing to know which kind of send it was. */
static void tx_claim(struct d2d_engine *e, uint32_t kind)
{
	e->tx_inflight++;
	((uint32_t *)e->pending_addr)[e->pend_tail++ % D2D_PENDING] = kind;
}

/* Forward one byte range to a peer through a send buffer.
 *
 * Sending straight out of the receive buffer saves the copy but holds that buffer until the send
 * completes, and with symmetric traffic both sides can end up holding every buffer waiting for a
 * completion the peer can no longer produce: measured as a hard stall after about 1255 buffers with
 * no error anywhere. Copying decouples them, so it is the path that always makes progress; see
 * forward_in_place for when the copy is skipped. */
static void forward(struct d2d_engine *e, uint32_t peer, uint32_t idx, uint64_t off, uint64_t len)
{
	uint32_t s;
	uint64_t dst;

	if (!tx_available(e)) {
		e->tx_full++;
		return;
	}
	s = e->next_tx++ % e->ntx;
	dst = e->tx_base + (uint64_t)s * e->buf_size;

	copy_bytes(dst, e->rx_base + (uint64_t)idx * e->buf_size + off, len);
	/* Terminate it, or the receiver walks past this run into the previous contents. */
	if (len + sizeof(struct request_head) <= e->buf_size)
		*(uint64_t *)(dst + len) = 0;
	tx_claim(e, D2D_TX);
	doca_dpa_dev_rdma_post_send_imm(e->rdma[peer], 0, e->mmap, dst, e->buf_size, 0,
					DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
	e->forwards++;
}

/* Holding every receive buffer for an outstanding forward is what deadlocked the symmetric case:
 * neither side had a receive posted, so neither side's sends could complete. Keeping half the pool
 * posted at all times means a peer always has somewhere to deliver, so a held buffer is always
 * released eventually. */
static int can_hold_more(const struct d2d_engine *e)
{
	return deferred(e) + e->held + 1 <= e->defer_max;
}

/* Send the whole receive buffer where it lies. Returns non-zero when the buffer is now held, so the
 * caller must not re-post it: the completion path does that once the send is done. */
static int forward_in_place(struct d2d_engine *e, uint32_t peer, uint32_t idx)
{
	if (!sq_available(e)) {
		e->tx_full++;
		return 0;
	}
	e->refcount[idx] = 1;
	e->held++;
	((uint32_t *)e->pending_addr)[e->pend_tail++ % D2D_PENDING] = idx;
	doca_dpa_dev_rdma_post_send_imm(e->rdma[peer], 0, e->mmap,
					e->rx_base + (uint64_t)idx * e->buf_size, e->buf_size, 0,
					DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
	e->forwards++;
	e->zerocopy++;
	return 1;
}

/* Walk the records and forward each run of consecutive records sharing a destination as one send.
 * Runs matter: buddy aggregates many small requests per buffer, and one send per record would cost
 * far more than the walk itself. */
static void route_buffer(struct d2d_engine *e, uint32_t idx, uint64_t len)
{
	const uint64_t buf = e->rx_base + (uint64_t)idx * e->buf_size;
	uint64_t pos = 0, run_start = 0;
	uint32_t run_peer = D2D_LOCAL;
	int have_run = 0, held_here = 0;

	while (pos + sizeof(struct request_head) <= len) {
		const struct request_head *h = (const struct request_head *)(buf + pos);
		const uint64_t rec = sizeof(*h) + h->size;
		uint32_t peer;

		/* A zero size is the end of the message, not a malformed record. A completion does not
		 * report how many bytes arrived, so the sender leaves this terminator and the walk stops
		 * there instead of running into whatever the buffer held before. */
		if (h->size == 0) break;
		if (pos + rec > len) {
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
		if (run_peer == D2D_LOCAL)
			e->local++;
		/* The whole buffer is one run to one peer, which is buddy's common case: send it as it
		 * lies, terminator and all, instead of copying it to a transmit slot. */
		else if (run_start == 0 && !e->no_zerocopy && can_hold_more(e))
			held_here = forward_in_place(e, run_peer, idx);
		else
			forward(e, run_peer, idx, run_start, pos - run_start);
	}

	{
		/* A held buffer goes back only when its forward completes; anything else is free now
		 * because the forward copied out of it. */
		if (!held_here) repost(e, idx);
		/* Tell the peer the buffer is free again. A side that only receives never sends, so
		 * nothing carries that back and it stops after the initially advertised supply:
		 * measured as exactly 128 posted plus 383 re-posts, every run. buddy's ACK does this
		 * job, which is why the credit protocol has to live here too. */
		if (e->ack_len && ++e->freed >= D2D_ACK_BATCH && sq_available(e)) {
			e->freed = 0;
			((uint32_t *)e->pending_addr)[e->pend_tail++ % D2D_PENDING] = D2D_ACK;
			doca_dpa_dev_rdma_post_send_imm(e->rx_rdma, 0, e->mmap, e->tx_base,
							e->ack_len, 0,
							DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
		}
	}
}

static void post_generated(struct d2d_engine *e)
{
	uint32_t s;

	if (!tx_available(e)) {
		e->tx_full++;
		return;
	}
	s = e->next_tx++ % e->ntx;
	e->tx_msgs++;
	tx_claim(e, D2D_GEN);
	doca_dpa_dev_rdma_post_send_imm(e->rdma[0], 0, e->mmap,
					e->tx_base + (uint64_t)s * e->buf_size, e->tx_len, 0,
					DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
}

/* Route a buffer now if there is room, otherwise park it. A parked buffer keeps its records and
 * keeps its receive slot out of circulation, which is what tells the peer to slow down. */
static void route_or_defer(struct d2d_engine *e, uint32_t idx)
{
	if (route_capacity(e)) {
		route_buffer(e, idx, e->buf_size);
		return;
	}
	if (can_defer(e)) {
		((uint32_t *)e->defer_addr)[e->defer_tail++ % D2D_MAX_BUFS] = idx;
		return;
	}
	/* Neither room to route nor room to park. Re-posting loses the records, so this must stay at
	 * zero; a non-zero count means the back-pressure above is not holding. */
	e->tx_full++;
	repost(e, idx);
}

static void drain_deferred(struct d2d_engine *e)
{
	while (e->defer_head != e->defer_tail && route_capacity(e)) {
		const uint32_t idx = ((uint32_t *)e->defer_addr)[e->defer_head++ % D2D_MAX_BUFS];
		route_buffer(e, idx, e->buf_size);
	}
	/* Give the generator its window back only once routing is comfortably clear, not the instant
	 * the queue empties: resuming on the first free slot just re-fills the pool and parks the next
	 * buffer, and the two oscillate instead of the backlog draining. */
	while (e->gen_paused && e->defer_head == e->defer_tail && e->held == 0 && e->tx_len &&
	       e->tx_inflight * 4 <= e->ntx && sq_available(e)) {
		e->gen_paused--;
		post_generated(e);
	}
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
				route_or_defer(e, idx);
				break;
			}
			case DOCA_DPA_DEV_COMP_SEND: {
				if (e->pend_head != e->pend_tail) {
					const uint32_t idx =
						((uint32_t *)e->pending_addr)[e->pend_head++ %
								 D2D_PENDING];
					if (idx == D2D_ACK) {
						/* nothing to release */
					} else if (idx == D2D_TX) {
						if (e->tx_inflight) e->tx_inflight--;
					} else if (idx == D2D_GEN) {
						if (e->tx_inflight) e->tx_inflight--;
						/* Routing owns the transmit pool first. A generator that
						 * keeps its window full while buffers are parked starves
						 * the forwards that would unpark them, and the records in
						 * them are lost. */
						if (!e->tx_len) {
							/* not a generator */
						} else if (e->defer_head != e->defer_tail ||
							   !route_capacity(e)) {
							e->gen_paused++;
						} else {
							post_generated(e);
						}
					}
					else if (e->refcount[idx] && --e->refcount[idx] == 0) {
						if (e->held) e->held--;
						repost(e, idx);
					}
				}
				break;
			}
			default:
				/* Record when the first error hit, not just that it did: whether errors
				 * land in the first few hundred messages or under load decides whether
				 * this is bring-up or a race in the hot path. */
				if (!e->errors) {
					e->first_err_rx = e->rx_msgs;
					e->first_err_tx = e->tx_msgs;
				}
				e->errors++;
				e->last_err = t;
				/* A failed receive still consumed its queue entry, so give the buffer back
				 * and take it off the FIFO. Leaving it there shifts every later receive
				 * onto the wrong buffer, which is why one error used to kill the engine
				 * for the rest of the run rather than costing a single message. */
				if (t == DOCA_DPA_DEV_COMP_RECV_ERR && e->rx_head != e->rx_tail) {
					const uint32_t bad =
						((uint32_t *)e->rxfifo_addr)[e->rx_head++ %
									     D2D_PENDING];
					repost(e, bad);
				}
				break;
			}
		}

		drain_deferred(e);
		doca_dpa_dev_completion_request_notification(e->comp);
		if (!drained) break;
	}

	doca_dpa_dev_thread_reschedule();
}
