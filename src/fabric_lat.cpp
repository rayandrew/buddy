/* Per-operation cost of whichever D2D fabric this tree was configured with.
 *
 * Built in both fabric trees, so DocaRdma and DpaFabric are measured by identical code.
 *
 *   local   submit -> our own send completion. No peer involved, so this is what the fabric costs
 *           to get an operation onto the wire and report it back.
 *   rtt     submit -> the peer's reply arrives. Adds the peer's receive and send.
 *
 * LAT_WINDOW sets how many messages are in flight per thread and LAT_THREADS how many threads share
 * the fabric, so the buddy configurations that differ most between the legs (depth 2 against depth
 * 128, one routing thread against two) can be reproduced without the proxy protocol on top.
 *
 *   node A: fabric-lat --server        node B: fabric-lat --peer <A-ip>
 */
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef FABRIC_DPA
#include "rdma_dpa.h"
namespace buddy::rdma { using Fabric = DpaFabric; }
static const char *kFabric = "dpa";
#else
#include "rdma_doca.h"
namespace buddy::rdma { using Fabric = DocaRdma; }
static const char *kFabric = "doca";
#endif

#define PORT 18519

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

static double now_us()
{
	using clk = std::chrono::steady_clock;
	return std::chrono::duration<double, std::micro>(clk::now().time_since_epoch()).count();
}

/* The DPA reads and writes host memory through its own window, so payload written by ordinary Arm
 * stores may not be visible to it, and payload it wrote may still be stale in the Arm's cache.
 * LAT_PAYLOAD_COHERENT=0 turns this off to show what happens without it. */
static bool payload_maint()
{
#ifdef FABRIC_DPA
	static const bool on = !getenv("LAT_NO_PAYLOAD_MAINT");
	return on;
#else
	return false;
#endif
}

static void clean_range(void *p, size_t len)
{
	if (!payload_maint()) return;
	asm volatile("dsb sy" ::: "memory");
	for (char *q = (char *)p; q < (char *)p + len; q += 64)
		asm volatile("dc cvac, %0" ::"r"(q) : "memory");
	asm volatile("dsb sy" ::: "memory");
}

static void invalidate_range(void *p, size_t len)
{
	if (!payload_maint()) return;
	for (char *q = (char *)p; q < (char *)p + len; q += 64)
		asm volatile("dc civac, %0" ::"r"(q) : "memory");
	asm volatile("dsb sy" ::: "memory");
}

/* The E1 host-side knob is a busy wait: usleep overshoots by the timer slack, about 50 us on
 * the BlueField, so a 1 us sleep would not be one. */
static void pause_us(int us)
{
	const double until = now_us() + us;
	while (now_us() < until) {
	}
}

static int env_int(const char *k, int dflt)
{
	const char *e = getenv(k);
	return e && *e ? atoi(e) : dflt;
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

	const int rounds = env_int("LAT_ROUNDS", 2000);
	const int window = env_int("LAT_WINDOW", 1);
	const int nthr = env_int("LAT_THREADS", 1);
	const size_t msg = (size_t)env_int("LAT_SIZE", 4080);
	/* One slot per message, page aligned; a 4 KB slot carries the default 4080-byte message. */
	const size_t slot_bytes = (msg + 4095) & ~(size_t)4095;
	const int warmup = env_int("LAT_WARMUP", 0);
	/* The host-side knob of the E1 evaluation: a wait before each completion poll. The device
	 * finished at the same time; the program only looked later. */
	const int sleep_us = env_int("LAT_SLEEP_US", 0);

	/* One receive slot per in-flight message per thread, plus the same again for sends, so no two
	 * messages ever share a buffer. */
	const int nslot = window * nthr;
	const size_t len = (size_t)slot_bytes * nslot * 2;
	char *mem = (char *)aligned_alloc(64, len);
	memset(mem, 0, len);
	/* dc civac is clean-and-invalidate, the only cache op at EL0, so it writes a dirty line back
	 * before invalidating it. memset leaves every line dirty, and the first invalidate of a
	 * receive buffer would flush those stale zeros over data the DPA had already delivered. Clean
	 * the whole region once so no dirty line ever meets a device write. */
	for (char *q = mem; q < mem + len; q += 64)
		asm volatile("dc civac, %0" ::"r"(q) : "memory");
	asm volatile("dsb sy" ::: "memory");

	buddy::rdma::Fabric f(1, mem, len, 1);
	int sock = peer_sock(server ? NULL : peer);
	if (sock < 0) { printf("socket failed\n"); return 1; }
	f.connect(0, sock, server);
	f.wait_connected();

	const size_t recv_off = 0, send_off = (size_t)slot_bytes * nslot;
	for (int i = 0; i < nslot; i++) f.post_recv(0, recv_off + (size_t)i * slot_bytes, slot_bytes, 1000 + i);
	{
		char sync = 1;
		if (write(sock, &sync, 1) != 1 || read(sock, &sync, 1) != 1) return 1;
	}
	/* Every message carries a unique id, and the client checks it gets each one back exactly once.
	 * The id is deliberately not the slot index: a message lands in whichever receive buffer is
	 * next on the receiving side, which has nothing to do with the buffer it was sent from. */
	std::atomic<uint64_t> next_id{1};
	std::vector<uint64_t> outstanding(nslot, 0);
	std::atomic<long> bad{0};
	std::mutex seen_lock;
	std::vector<bool> seen;
	auto stamp = [&](int slot) {
		uint64_t *p = (uint64_t *)(mem + send_off + (size_t)slot * slot_bytes);
		p[0] = next_id.fetch_add(1, std::memory_order_relaxed);
		outstanding[slot] = p[0];
		clean_range(p, msg);
	};
	auto payload_id = [&](int slot) {
		invalidate_range(mem + recv_off + (size_t)slot * slot_bytes, msg);
		return *(const uint64_t *)(mem + recv_off + (size_t)slot * slot_bytes);
	};
	auto verify = [&](int slot) {
		const uint64_t id = payload_id(slot);
		std::lock_guard<std::mutex> g(seen_lock);
		if (id == 0 || id >= seen.size()) seen.resize(id + 1024, false);
        if (id == 0 || seen[id]) {
			if (bad.fetch_add(1, std::memory_order_relaxed) < 8)
				fprintf(stderr, "CORRUPT slot=%d id=%lu %s\n", slot, id,
					id == 0 ? "(zero)" : "(duplicate)");
		} else {
			seen[id] = true;
		}
	};
	seen.resize(4096, false);
	for (int i = 0; i < nslot; i++) stamp(i);

	std::vector<std::thread> threads;
	std::vector<std::vector<double>> per_thread(nthr);
	const double t_start = now_us();
	std::atomic<long> done{0};

	/* No lock here: both fabrics make concurrent poll safe themselves, DocaRdma with a per-lane
	 * mutex and DpaFabric with a compare-exchange, so wrapping one would serialise the lock-free
	 * side and measure the wrapper instead of the fabric. */
	auto drain_one = [&](buddy::rdma::Fabric::completion *c) { return f.poll(0, c); };

	/* A stall shows which side stopped: submits not drained means the kernel is stuck, submits
	 * drained with nothing published means the operation never came back. Armed on both ends,
	 * because a client waiting on replies says nothing about why the server stopped sending. */
	std::atomic<bool> finished{false};
	std::thread watchdog([&] {
		long last = -1;
		for (int quiet = 0; !finished.load(std::memory_order_relaxed);) {
			usleep(500000);
			const long d = done.load(std::memory_order_relaxed);
			if (d != last) { last = d; quiet = 0; continue; }
			if (++quiet != 10) continue;
			quiet = 0;
#ifdef FABRIC_DPA
			const auto st = f.snapshot(0);
			fprintf(stderr,
				"STALL %s done=%ld host[sub_tail=%lu sub_consumed=%lu ring_head=%lu "
				"head_seq=%lu posted=%lu completed=%lu] "
				"kernel[sub_head=%lu tail=%lu errors=%lu last_err=%lu "
				"rwrid=%lu/%lu wrid=%lu/%lu]\n",
				server ? "server" : "client", d, st.sub_tail, st.sub_consumed,
				st.ring_head, st.head_seq, st.posted, st.completed,
				st.k_sub_head, st.k_tail, st.k_errors, st.k_last_err,
				st.k_rwrid_head, st.k_rwrid_tail, st.k_wrid_head, st.k_wrid_tail);
#else
			fprintf(stderr, "STALL %s done=%ld\n", server ? "server" : "client", d);
#endif
		}
	});

	/* The server echoes until the client says it is finished; the client times. Replies arrive in
	 * order on a reliable connection, so with a window the Nth reply belongs to the Nth send. */
	if (server) {
		std::atomic<bool> stop{false};
		std::thread waiter([&] {
			char sync;
			if (read(sock, &sync, 1) == 1) stop.store(true, std::memory_order_relaxed);
		});
		for (int t = 0; t < nthr; t++) {
			threads.emplace_back([&, t] {
				buddy::rdma::Fabric::completion c;
				while (!stop.load(std::memory_order_relaxed)) {
					if (!drain_one(&c)) continue;
					if (c.op != buddy::rdma::Fabric::OP_RECV) continue;
					const uint64_t slot = c.wr_id - 1000;
					const uint64_t id = payload_id((int)slot);
					*(uint64_t *)(mem + send_off + slot * slot_bytes) = id;
					clean_range(mem + send_off + slot * slot_bytes, msg);
					/* The id rides in the immediate too, so a trace of the NIC's completions
					 * can pair a message with its reply across the two nodes. */
					f.send_imm(0, 0, (uint32_t)id, send_off + slot * slot_bytes, msg, 500 + slot);
					f.post_recv(0, recv_off + slot * slot_bytes, slot_bytes, 1000 + slot);
					done.fetch_add(1, std::memory_order_relaxed);
				}
			});
		}
		for (auto &th : threads) th.join();
		waiter.join();
		finished.store(true, std::memory_order_relaxed);
		watchdog.join();
		close(sock);
		free(mem);
		return 0;
	}

	/* A slot is issued again only after its reply has returned it to the pool, so a timestamp is
	 * never overwritten while its message is outstanding. The pool is shared because any thread may
	 * drain any thread's reply; per-thread pools strand a slot whose reply another thread took. */
	std::vector<double> issued(nslot, 0.0);
	std::vector<int> free_slots;
	std::mutex pool_lock, lat_lock;
	std::atomic<int> claimed{0};
	const int total = (rounds + warmup) * nthr;
	std::atomic<double> t_warm{t_start};
	for (int i = 0; i < nslot; i++) free_slots.push_back(i);

	for (int t = 0; t < nthr; t++) {
		threads.emplace_back([&] {
			buddy::rdma::Fabric::completion c;
			std::vector<double> mine;
			mine.reserve(total / nthr + 1);

			while (done.load(std::memory_order_relaxed) < total) {
				bool posted = false;
				for (;;) {
					int slot = -1;
					{
						std::lock_guard<std::mutex> g(pool_lock);
						if (free_slots.empty()) break;
						if (claimed.fetch_add(1, std::memory_order_relaxed) >= total) {
							claimed.fetch_sub(1, std::memory_order_relaxed);
							break;
						}
						slot = free_slots.back();
						free_slots.pop_back();
					}
					/* A fresh id per message, in the payload and the immediate: the
					 * server echoes it, so the NIC's completions on both nodes carry
					 * one key per round trip. */
					stamp(slot);
					issued[slot] = now_us();
					f.send_imm(0, 0, (uint32_t)outstanding[slot],
						   send_off + (size_t)slot * slot_bytes, msg, 500 + slot);
					posted = true;
				}
				/* Once per send, not per poll: the time the host looks away after posting. */
				if (sleep_us && posted) pause_us(sleep_us);
				if (!drain_one(&c)) continue;
				if (c.op != buddy::rdma::Fabric::OP_RECV) continue;
				const int slot = (int)(c.wr_id - 1000);
				mine.push_back(now_us() - issued[slot]);
				f.post_recv(0, recv_off + (size_t)slot * slot_bytes, slot_bytes, 1000 + slot);
				{
					std::lock_guard<std::mutex> g(pool_lock);
					free_slots.push_back(slot);
				}
				if (done.fetch_add(1, std::memory_order_relaxed) + 1 == warmup * nthr)
					t_warm.store(now_us(), std::memory_order_relaxed);
			}
			std::lock_guard<std::mutex> g(lat_lock);
			per_thread[0].insert(per_thread[0].end(), mine.begin(), mine.end());
		});
	}


	for (auto &th : threads) th.join();
	const double elapsed = now_us() - t_start;
	const double timed = now_us() - t_warm.load(std::memory_order_relaxed);
	const long timed_ops = done.load() - (long)warmup * nthr;
	finished.store(true, std::memory_order_relaxed);
	watchdog.join();

	std::vector<double> all;
	for (auto &v : per_thread) all.insert(all.end(), v.begin() + v.size() / 10, v.end());
	std::sort(all.begin(), all.end());
	if (!all.empty())
		printf("%-4s w=%-4d thr=%-2d n=%-7zu min %8.2f  p50 %8.2f  p99 %8.2f us   %8.0f msg/s  corrupt=%ld\n",
		       kFabric, window, nthr, all.size(), all.front(), all[all.size() / 2],
		       all[all.size() * 99 / 100], done.load() * 1e6 / elapsed,
		       bad.load(std::memory_order_relaxed));
	/* The one line every E1 program prints, over the operations after the warm-up. */
	printf("ops=%ld bytes=%zu wall_ns=%.0f sleep_us=%d window=%d\n", timed_ops, msg,
	       timed * 1e3 / (timed_ops > 0 ? timed_ops : 1), sleep_us, window);
	fflush(stdout);

	{
		char sync = 1;
		if (write(sock, &sync, 1) != 1) return 1;
	}
	close(sock);
	free(mem);
	return 0;
}
