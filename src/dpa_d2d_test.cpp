/* Drives DpaD2D across two nodes: the device generates load, receives, walks the records and
 * forwards or counts them, and the Arm only samples counters.
 *
 * D2D_FORWARD=1 routes every rank at the peer, so each buffer is parsed and forwarded back out
 * without a copy. Left unset, records are counted as local, which measures receive and route alone.
 *
 *   node A: dpa-d2d --server        node B: dpa-d2d --peer <A-ip>
 */
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "dpa_d2d.h"
#include "sockets.h"
#include "util.h"

#define PORT 18523

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

static int env_int(const char *k, int d)
{
	const char *e = getenv(k);
	return e && *e ? atoi(e) : d;
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

	const unsigned engines = (unsigned)env_int("D2D_ENGINES", 1);
	const unsigned bufs = (unsigned)env_int("D2D_BUFS", 32);
	const size_t size = (size_t)env_int("D2D_SIZE", 4096);
	const unsigned window = (unsigned)env_int("D2D_WINDOW", 16);
	const unsigned records = (unsigned)env_int("D2D_RECORDS", 32);
	const int seconds = env_int("D2D_SECONDS", 5);

	buddy::rdma::DpaD2D d2d(engines, bufs, size);
	if (env_int("D2D_FORWARD", 0))
		for (int r = 0; r < (int)buddy::rdma::DpaD2D::kMaxRanks; r++) d2d.route_to_peer(r);

	int sock = peer_sock(server ? NULL : peer);
	if (sock < 0) FAIL("socket failed");
	d2d.connect(sock, server);
	d2d.wait_connected();
	d2d.start();

	/* Both sides must be armed before either generates, or the first message meets an empty
	 * receive queue. */
	{
		char sync = 1;
		buddy::full_write(sock, &sync, 1);
		buddy::full_read(sock, &sync, 1);
	}
	if (!server) d2d.generate(window, records);

	auto prev = d2d.sample();
	for (int i = 0; i < seconds; i++) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const auto s = d2d.sample();
		printf("dpa-d2d %s t=%2ds  rx %8lu/s  tx %8lu/s  %10lu rec/s  wakes=%-10lu fwd=%-9lu local=%-9lu bad=%lu errors=%lu last=0x%lx first_err@rx=%lu\n",
		       server ? "server" : "client", i + 1, s.rx_msgs - prev.rx_msgs,
		       s.tx_msgs - prev.tx_msgs, s.records - prev.records, s.wakes, s.forwards,
		       s.local, s.bad_records, s.errors, s.last_err, s.first_err_rx);
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
