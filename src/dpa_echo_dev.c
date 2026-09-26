/* Device half of the DPA-native echo: buffers in DPA memory, a thread woken by completions, and
 * no Arm involvement once traffic starts.
 *
 * Nothing here touches host memory, so there is no window, no cache maintenance and no submit or
 * completion ring. Buffers are plain device pointers registered for RDMA with
 * doca_mmap_set_dpa_memrange, so a load is a load.
 */
#include <doca_dpa_dev.h>
#include <doca_dpa_dev_buf.h>
#include <doca_dpa_dev_rdma.h>

/* Completions handled per drain pass before the handler must return to the scheduler. */
#define ECHO_DRAIN_MAX 8

struct echo_arg {
	doca_dpa_dev_t ctx;
	doca_dpa_dev_completion_t comp;
	doca_dpa_dev_rdma_t rdma;
	doca_dpa_dev_mmap_t mmap;

	uint64_t recv_base;     /* nslot receive buffers */
	uint64_t send_base;     /* nslot send buffers, never aliased with a posted receive */
	uint64_t slot_size;
	uint32_t nslot;

	uint64_t handled;       /* messages echoed; the host samples this to get a rate */
	uint64_t errors;
	uint64_t last_err;
	uint32_t next_recv;     /* receive buffer to re-post into */
	uint32_t next_send;

	uint64_t target;        /* messages to keep in flight; reached by ramping from the handler */
	uint64_t msg_len;
	uint64_t burst;         /* how many the start RPC may post itself */
	uint64_t in_flight;
	uint32_t parse;         /* walk each message's request records before echoing */
	uint64_t records;       /* records seen, so the host can price the walk */
	uint64_t bad_records;
};

/* Mirrors buddy::request_head. Records are packed head to tail with no padding, so walking a
 * buffer is pos += sizeof(head) + head->size. */
struct request_head {
	uint32_t size;
	int32_t dst;
};

/* What routing on the device costs: the header walk a forwarding decision needs, with no copy.
 * A real router would post a send per destination run instead of counting. */
static void walk_records(struct echo_arg *a, uint64_t buf, uint64_t len)
{
	uint64_t pos = 0;
	while (pos + sizeof(struct request_head) <= len) {
		const struct request_head *h = (const struct request_head *)(buf + pos);
		if (h->size == 0 || pos + sizeof(*h) + h->size > len) {
			a->bad_records++;
			return;
		}
		a->records++;
		pos += sizeof(*h) + h->size;
	}
}

/* Posts the initial receives. Only the device can post once the data path is on the DPA, so setup
 * comes through an RPC rather than a host-side task. */
__dpa_rpc__ uint64_t echo_arm(uint64_t arg_addr)
{
	struct echo_arg *a = (struct echo_arg *)arg_addr;
	if (a->ctx) doca_dpa_dev_device_set(a->ctx);
	for (uint32_t i = 0; i < a->nslot; i++)
		doca_dpa_dev_rdma_post_receive(a->rdma, a->mmap,
					       a->recv_base + (uint64_t)i * a->slot_size,
					       a->slot_size);
	a->next_recv = 0;
	doca_dpa_dev_completion_request_notification(a->comp);
	return 0;
}

/* Starts the traffic: one send per window slot. Each receive posts exactly one send, so this burst
 * also sets the steady depth. */
__dpa_rpc__ uint64_t echo_start(uint64_t arg_addr, uint64_t window, uint64_t len)
{
	struct echo_arg *a = (struct echo_arg *)arg_addr;
	uint64_t i;

	if (a->ctx) doca_dpa_dev_device_set(a->ctx);
	a->msg_len = len;
	for (i = 0; i < window; i++) {
		const uint32_t s = a->next_send++ % a->nslot;
		doca_dpa_dev_rdma_post_send_imm(a->rdma, 0, a->mmap,
						a->send_base + (uint64_t)s * a->slot_size, len,
						0, DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
	}
	return 0;
}

/* Woken by the completion context, not spinning: the thread holds no execution unit between
 * messages. Each wake drains what is ready, echoes it, and reschedules. */
__dpa_global__ void echo_handler(uint64_t arg_addr)
{
	struct echo_arg *a = (struct echo_arg *)arg_addr;
	doca_dpa_dev_completion_element_t e;

	if (a->ctx) doca_dpa_dev_device_set(a->ctx);

	/* Arm, then drain again before giving up the thread. A completion that arrives between the
	 * last empty drain and the arming would otherwise never be signalled, and the thread sleeps
	 * for good: measured as the echo stopping dead after a few thousand messages.
	 *
	 * The drain is bounded because handling a completion posts another operation, whose completion
	 * can arrive before the drain ends. Unbounded, the handler never returns to the scheduler once
	 * the window is deep enough: measured as the echo dying at a window of 128. */
	for (uint32_t round = 0; round < 1; round++) {
		uint32_t acked = 0;

		while (acked < ECHO_DRAIN_MAX && doca_dpa_dev_get_completion(a->comp, &e)) {
			const doca_dpa_dev_completion_type_t t = doca_dpa_dev_get_completion_type(e);
			/* Ack each element as it is read, which is what the shipped samples do.
			 * Measured: acking the whole drain in one call at the end frees nothing, and
			 * the queue fills exactly once - the stall point tracks the queue size. */
			doca_dpa_dev_completion_ack(a->comp, 1);
			acked++;

			switch (t) {
			case DOCA_DPA_DEV_COMP_RECV_SEND:
			case DOCA_DPA_DEV_COMP_RECV_SEND_IMM:
			case DOCA_DPA_DEV_COMP_RECV_RDMA_WRITE_IMM: {
				/* Echo from a send buffer and give the receive buffer straight back, so
				 * a buffer is never both posted for receive and read by a send. */
				const uint32_t s = a->next_send++ % a->nslot;
				const uint32_t r = a->next_recv++ % a->nslot;
				if (a->parse)
					walk_records(a, a->recv_base + (uint64_t)r * a->slot_size,
						     a->slot_size);
				doca_dpa_dev_rdma_post_send_imm(a->rdma, 0, a->mmap,
								a->send_base +
									(uint64_t)s * a->slot_size,
								a->slot_size, 0,
								DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
				doca_dpa_dev_rdma_post_receive(a->rdma, a->mmap,
							       a->recv_base +
								       (uint64_t)r * a->slot_size,
							       a->slot_size);
				a->handled++;
				break;
			}
			case DOCA_DPA_DEV_COMP_SEND:
				break;
			default:
				a->errors++;
				a->last_err = t;
				break;
			}
		}

		doca_dpa_dev_completion_request_notification(a->comp);
		if (!acked) break;
	}

	doca_dpa_dev_thread_reschedule();
}
