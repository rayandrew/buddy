#include <chrono>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <rdma/rdma_cma.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_rdma.h>

#include "dpa_hostleg.h"
#include "util.h"

namespace buddy::rdma {

static const uint32_t kPerms = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
			       DOCA_ACCESS_FLAG_RDMA_WRITE;

HostLeg::HostLeg(doca_dpa *dpa, doca_dev *dev) : dpa(dpa), dev(dev)
{
	CHECK_DOCA(doca_rdma_create(dev, &rdma));
	ctx = doca_rdma_as_ctx(rdma);
	CHECK_DOCA(doca_ctx_set_datapath_on_dpa(ctx, dpa));
	CHECK_DOCA(doca_rdma_set_permissions(rdma, kPerms));
	CHECK_DOCA(doca_rdma_set_grh_enabled(rdma, 1));
	CHECK_DOCA(doca_rdma_set_max_num_connections(rdma, 1));
	CHECK_DOCA(doca_rdma_set_rnr_retry_count(rdma, 7));
	CHECK_DOCA(doca_rdma_set_send_queue_size(rdma, 1024));
	{
		const doca_error_t rq = doca_rdma_set_recv_queue_size(rdma, 1024);
		if (rq != DOCA_SUCCESS && rq != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(rq));
		const doca_error_t bl = doca_rdma_task_receive_set_dst_buf_list_len(rdma, 1);
		if (bl != DOCA_SUCCESS && bl != DOCA_ERROR_NOT_SUPPORTED)
			FAIL("doca " << doca_error_get_descr(bl));
	}
}

void HostLeg::attach(doca_dpa_completion *comp)
{
	CHECK_DOCA(doca_rdma_dpa_completion_attach(rdma, comp));
	CHECK_DOCA(doca_ctx_start(ctx));
	CHECK_DOCA(doca_rdma_get_dpa_handle(rdma, &handle));
}

void HostLeg::listen(uint16_t port)
{
	ec = rdma_create_event_channel();
	if (!ec) FAIL("rdma_create_event_channel failed");
	if (rdma_create_id(ec, &listen_id, nullptr, RDMA_PS_TCP)) FAIL("rdma_create_id failed");

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (rdma_bind_addr(listen_id, (sockaddr *)&addr)) FAIL("rdma_bind_addr failed");
	if (rdma_listen(listen_id, 1)) FAIL("rdma_listen failed");
}

bool HostLeg::accept_one(double timeout_s)
{
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
	while (std::chrono::steady_clock::now() < deadline) {
		rdma_cm_event *ev = nullptr;
		if (rdma_get_cm_event(ec, &ev)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			continue;
		}
		const rdma_cm_event_type t = ev->event;
		rdma_cm_id *id = ev->id;
		rdma_ack_cm_event(ev);

		if (t == RDMA_CM_EVENT_CONNECT_REQUEST) {
			CHECK_DOCA(doca_rdma_bridge_prepare_connection(rdma, id, &conn));
			CHECK_DOCA(doca_rdma_bridge_accept(rdma, nullptr, 0, conn));
			continue;
		}
		if (t == RDMA_CM_EVENT_ESTABLISHED) return true;
		if (t == RDMA_CM_EVENT_REJECTED || t == RDMA_CM_EVENT_CONNECT_ERROR ||
		    t == RDMA_CM_EVENT_UNREACHABLE)
			FAIL("host leg connection failed, cm event " << (int)t);
	}
	return false;
}

HostLeg::~HostLeg()
{
	if (ctx) doca_ctx_stop(ctx);
	if (rdma) doca_rdma_destroy(rdma);
	if (listen_id) rdma_destroy_id(listen_id);
	if (ec) rdma_destroy_event_channel(ec);
}

} // namespace buddy::rdma
