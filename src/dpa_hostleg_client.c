#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SLOT 32768
#define NREC 32
#define WINDOW 32
#define NRECV 256

/* Mirrors buddy::request_head: records packed head to tail, a zero size ending the message. */
struct request_head {
	uint32_t size;
	int32_t dst;
};

static void fill_records(char *buf, size_t cap, unsigned n)
{
	const size_t each = cap / n;
	size_t pos = 0;
	unsigned i;

	for (i = 0; i < n && pos + each <= cap; i++, pos += each) {
		struct request_head h;
		h.size = (uint32_t)(each - sizeof(h));
		h.dst = 0;
		memcpy(buf + pos, &h, sizeof(h));
	}
	if (pos + sizeof(struct request_head) <= cap)
		memset(buf + pos, 0, sizeof(struct request_head));
}

int main(int argc, char **argv)
{
	const char *peer = argc > 1 ? argv[1] : NULL;
	const uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 18525;
	const int msgs = argc > 3 ? atoi(argv[3]) : 8;
	const int recv_mode = argc > 4 && !strcmp(argv[4], "recv");
	const long idle_s = argc > 5 ? atol(argv[5]) : 30;
	struct rdma_event_channel *ec;
	struct rdma_cm_id *id;
	struct rdma_cm_event *ev;
	struct sockaddr_in addr;
	struct ibv_qp_init_attr qp_attr;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_mr *mr;
	char *buf, *rbuf;
	struct ibv_mr *rmr;

	if (!peer) { printf("usage: %s <dpu-ip> [port] [msgs] [send|recv] [idle-secs]\n", argv[0]); return 1; }

	ec = rdma_create_event_channel();
	if (!ec || rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) { perror("rdma_create_id"); return 1; }

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, peer, &addr.sin_addr);

	if (rdma_resolve_addr(id, NULL, (struct sockaddr *)&addr, 5000)) {
		perror("rdma_resolve_addr");
		return 1;
	}
	if (rdma_get_cm_event(ec, &ev)) { perror("addr event"); return 1; }
	if (ev->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
		printf("RESULT: address not resolved, event %d\n", ev->event);
		return 2;
	}
	rdma_ack_cm_event(ev);

	if (rdma_resolve_route(id, 5000)) { perror("rdma_resolve_route"); return 1; }
	if (rdma_get_cm_event(ec, &ev)) { perror("route event"); return 1; }
	if (ev->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
		printf("RESULT: route not resolved, event %d\n", ev->event);
		return 2;
	}
	rdma_ack_cm_event(ev);

	pd = ibv_alloc_pd(id->verbs);
	cq = ibv_create_cq(id->verbs, 16, NULL, NULL, 0);
	buf = aligned_alloc(64, SLOT);
	memset(buf, 0, SLOT);
	fill_records(buf, SLOT, NREC);
	mr = ibv_reg_mr(pd, buf, SLOT, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	rbuf = aligned_alloc(64, (size_t)SLOT * NRECV);
	memset(rbuf, 0, (size_t)SLOT * NRECV);
	rmr = ibv_reg_mr(pd, rbuf, (size_t)SLOT * NRECV,
			 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!pd || !cq || !mr || !rmr) { printf("RESULT: verbs setup failed\n"); return 1; }

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.send_cq = cq;
	qp_attr.recv_cq = cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = WINDOW + 8;
	qp_attr.cap.max_recv_wr = NRECV + 8;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;
	if (rdma_create_qp(id, pd, &qp_attr)) { perror("rdma_create_qp"); return 1; }

	/* Receives must be posted before the peer can send, so they go up before the connect. */
	if (recv_mode) {
		int i;
		for (i = 0; i < NRECV; i++) {
			struct ibv_sge sge = {(uint64_t)(rbuf + (size_t)i * SLOT), SLOT, rmr->lkey};
			struct ibv_recv_wr rwr, *rbad = NULL;
			memset(&rwr, 0, sizeof(rwr));
			rwr.wr_id = (uint64_t)i;
			rwr.sg_list = &sge;
			rwr.num_sge = 1;
			if (ibv_post_recv(id->qp, &rwr, &rbad)) {
				printf("RESULT: post_recv failed at %d\n", i);
				return 3;
			}
		}
	}

	{
		struct rdma_conn_param cp;
		memset(&cp, 0, sizeof(cp));
		cp.initiator_depth = 1;
		cp.responder_resources = 1;
		cp.retry_count = 7;
		cp.rnr_retry_count = 7;
		if (rdma_connect(id, &cp)) { perror("rdma_connect"); return 1; }
	}
	if (rdma_get_cm_event(ec, &ev)) { perror("connect event"); return 1; }
	if (ev->event != RDMA_CM_EVENT_ESTABLISHED) {
		printf("RESULT: connect refused, event %d\n", ev->event);
		return 2;
	}
	rdma_ack_cm_event(ev);
	printf("RESULT: host ibverbs connected to a DPA-datapath queue pair\n");

	if (recv_mode) {
		unsigned long got = 0, records = 0;
		struct timespec t0, now;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (;;) {
			struct ibv_wc wc;
			struct ibv_sge sge;
			struct ibv_recv_wr rwr, *rbad = NULL;
			char *slot;
			size_t pos = 0;

			if ((int)got >= msgs) break;
			if (ibv_poll_cq(cq, 1, &wc) <= 0) {
				clock_gettime(CLOCK_MONOTONIC, &now);
				/* Idle for the whole budget, not merely slow to start: the source may
				 * still be connecting when the first poll runs. */
				if (now.tv_sec - t0.tv_sec > idle_s) break;
				continue;
			}
			if (wc.status != IBV_WC_SUCCESS) {
				printf("RESULT: receive failed after %lu: %s\n", got,
				       ibv_wc_status_str(wc.status));
				return 3;
			}
			slot = rbuf + wc.wr_id * SLOT;
			while (pos + sizeof(struct request_head) <= SLOT) {
				const struct request_head *h =
					(const struct request_head *)(slot + pos);
				if (h->size == 0) break;
				if (pos + sizeof(*h) + h->size > SLOT) break;
				records++;
				pos += sizeof(*h) + h->size;
			}
			got++;
			clock_gettime(CLOCK_MONOTONIC, &t0);

			sge.addr = (uint64_t)slot;
			sge.length = SLOT;
			sge.lkey = rmr->lkey;
			memset(&rwr, 0, sizeof(rwr));
			rwr.wr_id = wc.wr_id;
			rwr.sg_list = &sge;
			rwr.num_sge = 1;
			ibv_post_recv(id->qp, &rwr, &rbad);
		}
		printf("RESULT: host received %lu message(s), %lu record(s)\n", got, records);
		rdma_disconnect(id);
		return got ? 0 : 4;
	}

	/* Sends, so the DPU's routing kernel sees traffic arrive from the host. */
	{
		struct ibv_sge sge = {(uint64_t)buf, SLOT, mr->lkey};
		struct ibv_send_wr wr, *bad = NULL;
		struct ibv_wc wc;
		int n = 0, spins = 0;

		memset(&wr, 0, sizeof(wr));
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr.opcode = IBV_WR_SEND;
		wr.send_flags = IBV_SEND_SIGNALED;
		/* Keep a window in flight. One at a time measures the round trip, not the rate the
		 * device can route at. */
		int posted = 0, done = 0;
		struct timespec t0, t1;
		double secs;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		while (done < msgs) {
			while (posted < msgs && posted - done < WINDOW) {
				if (ibv_post_send(id->qp, &wr, &bad)) {
					printf("RESULT: post_send failed after %d\n", posted);
					return 3;
				}
				posted++;
			}
			n = ibv_poll_cq(cq, 1, &wc);
			if (n <= 0) {
				if (++spins > 2000000000) {
					printf("RESULT: stalled after %d completions\n", done);
					return 3;
				}
				continue;
			}
			if (wc.status != IBV_WC_SUCCESS) {
				printf("RESULT: send %d failed: %s\n", done,
				       ibv_wc_status_str(wc.status));
				return 3;
			}
			done++;
			spins = 0;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		secs = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
		printf("RESULT: %d message(s) of %d bytes in %.3fs = %.0f msg/s, %.2f GB/s\n",
		       done, SLOT, secs, done / secs, done * (double)SLOT / secs / 1e9);
	}

	rdma_disconnect(id);
	return 0;
}
